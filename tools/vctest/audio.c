/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * audio.c - AUDIO: the firmware's audio service (AUDS) driven directly: OPEN,
 * CONTROL, CONFIG, chunks as WRITE plus a bulk transmit, START, drained,
 * STOP, CLOSE.  What comes back: the COMPLETEs' timing, underruns, the rate
 * consumed, what a STOP leaves.  TONE plays a tone instead of silence, WALK
 * one channel at a time (channel n beeps n times), RESTART plays again and
 * stops with chunks queued.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <dos/dos.h>
#include <exec/errors.h>
#include <exec/memory.h>

#include <args.h>
#include <kv.h>
#include <timing.h>
#include <auds_proto.h>

#include "commands.h"
#include "tone.h"

const char audio_template[] = "MS/N,DEST/N,RATE/N,CHANNELS/N,CHUNK/N,DEPTH/N,TONE/S,WALK/S,VOL/N,RESTART/S";

enum
{
	ARG_MS,
	ARG_DEST,
	ARG_RATE,
	ARG_CHANNELS,
	ARG_CHUNK,
	ARG_DEPTH,
	ARG_TONE,
	ARG_WALK,
	ARG_VOL,
	ARG_RESTART
};

#define AUDIO_STOP_QUIET_MS 300    /* after STOP, COMPLETEs are counted until this long passes without one */
#define AUDIO_GPFSEL4 0xf2200010UL /* GPIO 40-49's functions, where Emu68 maps the BCM2711's; the jack's PWM is 40 and 41 at ALT0 */
#define AUDIO_WALK_BEEP_MS 200     /* WALK: one beep, and the quiet after it */
#define AUDIO_WALK_GAP_MS 1000     /* WALK: between two channels */
#define AUDIO_WALK_LEAD_MS 2000    /* WALK: silence first, while the display or receiver takes up the new format */

/* One AUDIO run: what it plays and what came back */
struct audio_run
{
	ULONG dest, rate, volume; /* CONTROL's and CONFIG's */
	ULONG channels;           /* CONFIG's */
	ULONG frame_bytes;        /* 16 bits a channel */
	ULONG chunk_ms;
	ULONG chunk_bytes;        /* one WRITE's samples */
	ULONG depth;              /* chunks in flight at most */
	BOOL tone;                /* the tone, else silence */
	BOOL walk;                /* the tone in one channel at a time */
	ULONG at_ms;              /* where the next chunk starts, for the walk */
	ULONG phase;              /* the tone's, 16.16 of one period */
	const char *last;         /* the message sent last: what an unexpected RESULT answers */
	ULONG unexpected;         /* RESULTs nobody waited for, and messages of other types */
	ULONG sent, completed;    /* bytes */
	ULONG completes, flagged; /* COMPLETEs, those with AUDS_COMPLETE_UNDERRUN */
	ULONG first_bytes;        /* the first COMPLETE's count */
	ULONG bulk_max;           /* the longest bulk transmit, us */
	u32 t_start, t_first, t_prev, gap_max; /* START, the first and the latest COMPLETE, the longest gap */
	ULONG pins_playing;       /* GPFSEL4 once started */
};

/**
 * audio_send() - One message to the VPU, remembered as what a stray RESULT answers.
 * @t:    the tool
 * @a:    the run
 * @m:    the message
 * @what: its name
 *
 * Return: vq_do()'s error, 0 when sent.
 */
static LONG audio_send(struct tool *t, struct audio_run *a, const struct auds_msg *m, const char *what)
{
	a->last = what;
	LONG err = vq_do(&t->svc, CMD_WRITE, m, sizeof(*m), NULL);
	if (err != 0)
	{
		kv_num("error", err);
		note("%s: write error %ld\n", what, err);
	}
	return err;
}

/**
 * audio_pins() - The jack pins' functions: GPFSEL4, little-endian.
 *
 * Return: the register.
 */
static ULONG audio_pins(void)
{
	return VCHIQ_LE32(*(volatile ULONG *)AUDIO_GPFSEL4);
}

/**
 * audio_take() - The next message from the service.
 * @t:          the tool
 * @a:          the run
 * @timeout_ms: how long to wait
 * @result:     where a RESULT's value goes, or NULL when none is awaited
 *
 * A COMPLETE is counted; a RESULT goes to @result, or is noted with what it
 * answers when nothing waits for one.
 *
 * Return: the message's type, -1 after the timeout.
 */
static LONG audio_take(struct tool *t, struct audio_run *a, ULONG timeout_ms, LONG *result)
{
	UBYTE *data;
	ULONG len;
	if (!vq_next(&t->svc, &data, &len, timeout_ms))
		return -1;
	const struct auds_msg *m = (const struct auds_msg *)data;
	LONG type = (LONG)VCHIQ_LE32(m->type);
	if (type == AUDS_MSG_COMPLETE)
	{
		u32 now = get_time();
		ULONG count = VCHIQ_LE32(m->u.complete.count);
		if (a->completes++ == 0)
		{
			a->t_first = now;
			a->first_bytes = count & AUDS_COMPLETE_BYTES;
		}
		else if (now - a->t_prev > a->gap_max)
			a->gap_max = now - a->t_prev;
		a->t_prev = now;
		a->completed += count & AUDS_COMPLETE_BYTES;
		if (count & AUDS_COMPLETE_UNDERRUN)
			a->flagged++;
	}
	else if (type == AUDS_MSG_RESULT && result != NULL)
		*result = (LONG)VCHIQ_LE32((ULONG)m->u.result.success);
	else
	{
		a->unexpected++;
		if (type == AUDS_MSG_RESULT)
			note("RESULT %ld after %s\n", (LONG)VCHIQ_LE32((ULONG)m->u.result.success), a->last);
		else
			note("message type %ld after %s\n", type, a->last);
	}
	vq_consume(&t->svc);
	return type;
}

/**
 * audio_request() - A message the VPU answers (CONTROL, CONFIG, CLOSE).
 * @t:    the tool
 * @a:    the run
 * @m:    the message
 * @what: its name
 * @key:  the key its RESULT is printed under ("none" without one)
 *
 * Return: TRUE for RESULT 0.
 */
static BOOL audio_request(struct tool *t, struct audio_run *a, const struct auds_msg *m, const char *what, const char *key)
{
	if (audio_send(t, a, m, what) != 0)
		return FALSE;
	for (;;)
	{
		LONG result = 0;
		LONG type = audio_take(t, a, VQ_REPLY_TIMEOUT_MS, &result);
		if (type < 0)
		{
			kv_str(key, "none");
			return FALSE;
		}
		if (type == AUDS_MSG_RESULT)
		{
			kv_num(key, result);
			return result == 0;
		}
	}
}

/**
 * audio_walk_ms() - How long a walk over the channels takes.
 * @channels: how many
 *
 * Return: the lead, then channel n's n beeps and a gap each, in ms.
 */
static ULONG audio_walk_ms(ULONG channels)
{
	return AUDIO_WALK_LEAD_MS + channels * (channels + 1u) / 2u * 2u * AUDIO_WALK_BEEP_MS + channels * AUDIO_WALK_GAP_MS;
}

/**
 * audio_walk() - The channel a walk sounds at a moment.
 * @channels: how many
 * @ms:       milliseconds into the walk
 *
 * Return: the channel (from 0: channel 0 beeps once), -1 between beeps.
 */
static LONG audio_walk(ULONG channels, ULONG ms)
{
	if (ms < AUDIO_WALK_LEAD_MS)
		return -1;
	ms -= AUDIO_WALK_LEAD_MS;
	for (ULONG c = 0; c < channels; c++)
	{
		ULONG beeps_ms = (c + 1u) * 2u * AUDIO_WALK_BEEP_MS;
		if (ms < beeps_ms)
			return (ms / AUDIO_WALK_BEEP_MS) % 2u == 0 ? (LONG)c : -1;
		if (ms < beeps_ms + AUDIO_WALK_GAP_MS)
			return -1;
		ms -= beeps_ms + AUDIO_WALK_GAP_MS;
	}
	return -1;
}

/**
 * audio_fill() - One chunk of silence or tone, little-endian 16-bit.
 * @a:   the run
 * @buf: the chunk, cleared
 *
 * The tone goes to every channel, or to the walk's one.
 */
static void audio_fill(struct audio_run *a, UWORD *buf)
{
	if (!a->tone)
		return;
	LONG only = a->walk ? audio_walk(a->channels, a->at_ms) : -1;
	a->at_ms += a->chunk_ms;
	for (ULONG i = 0; i < a->chunk_bytes / a->frame_bytes; i++)
	{
		UWORD sample = VCHIQ_LE16((UWORD)tone_sample(&a->phase, a->rate, TONE_HZ));
		for (ULONG c = 0; c < a->channels; c++)
			*buf++ = !a->walk || (LONG)c == only ? sample : 0;
	}
}

/**
 * audio_chunk() - WRITE and its bulk transmit, which comes back when the VPU has the samples.
 * @t:   the tool
 * @a:   the run
 * @buf: the chunk's memory
 *
 * Return: TRUE when the VPU took it.
 */
static BOOL audio_chunk(struct tool *t, struct audio_run *a, UBYTE *buf)
{
	audio_fill(a, (UWORD *)buf);
	struct auds_msg m = {0};
	m.type = VCHIQ_LE32(AUDS_MSG_WRITE);
	m.u.write.count = VCHIQ_LE32(a->chunk_bytes);
	m.u.write.cookie1 = VCHIQ_LE32(AUDS_WRITE_COOKIE1);
	m.u.write.cookie2 = VCHIQ_LE32(AUDS_WRITE_COOKIE2);
	m.u.write.silence = VCHIQ_LE16(0);
	m.u.write.max_packet = VCHIQ_LE16(0); /* the samples follow as one bulk transmit */
	if (audio_send(t, a, &m, "WRITE") != 0)
		return FALSE;
	ULONG actual = 0;
	u32 t0 = get_time();
	LONG q = vq_do(&t->svc, NSCMD_VQ_BULKWRITE, buf, a->chunk_bytes, &actual);
	u32 us = get_time() - t0;
	if (us > a->bulk_max)
		a->bulk_max = us;
	if (q != 0 || actual != a->chunk_bytes)
	{
		kv_num("error", q);
		note("bulk transmit: error %ld, %lu of %lu bytes\n", q, actual, a->chunk_bytes);
		return FALSE;
	}
	a->sent += a->chunk_bytes;
	return TRUE;
}

/**
 * audio_play() - Queue up to depth chunks, START, then keep depth in flight until the chunks have gone.
 * @t:      the tool
 * @a:      the run
 * @buf:    the chunk's memory
 * @chunks: how many
 * @drain:  TRUE also waits until the VPU has consumed them all
 * @run:    the run's number, for its keys (run.N.*)
 *
 * Return: TRUE when every chunk went out (and, with @drain, came back consumed).
 */
static BOOL audio_play(struct tool *t, struct audio_run *a, UBYTE *buf, ULONG chunks, BOOL drain, ULONG run)
{
	a->sent = a->completed = a->completes = a->flagged = a->bulk_max = a->gap_max = 0;
	ULONG n = 0;
	for (; n < chunks && n < a->depth; n++)
		if (!audio_chunk(t, a, buf))
			return FALSE;
	struct auds_msg m = {0};
	m.type = VCHIQ_LE32(AUDS_MSG_START);
	if (audio_send(t, a, &m, "START") != 0)
		return FALSE;
	a->t_start = a->t_first = a->t_prev = get_time();
	a->first_bytes = 0;
	a->pins_playing = audio_pins();

	while (n < chunks || (drain && a->completed < a->sent))
	{
		if (n < chunks && (LONG)(a->sent - a->completed) <= (LONG)((a->depth - 1u) * a->chunk_bytes))
		{
			if (!audio_chunk(t, a, buf))
				return FALSE;
			n++;
			continue;
		}
		if (audio_take(t, a, VQ_REPLY_TIMEOUT_MS, NULL) < 0)
		{
			kv_str("fail", "no COMPLETE");
			note("no COMPLETE (have %lu of %lu bytes)\n", a->completed, a->sent);
			return FALSE;
		}
	}
	u32 us = get_time() - a->t_start;
	kv_unum(kv_at("run", run, "chunks"), n);
	kv_unum(kv_at("run", run, "chunk_bytes"), a->chunk_bytes);
	kv_unum(kv_at("run", run, "completed"), a->completed);
	kv_unum(kv_at("run", run, "us"), us);
	kv_unum(kv_at("run", run, "flagged"), a->flagged);
	if (a->completes > 0)
	{
		kv_unum(kv_at("run", run, "first_us"), a->t_first - a->t_start);
		kv_unum(kv_at("run", run, "first_bytes"), a->first_bytes);
		kv_unum(kv_at("run", run, "gap_us"), a->gap_max);
		kv_unum(kv_at("run", run, "bulk_us"), a->bulk_max);
	}
	ULONG ms = (a->t_prev - a->t_first) / 1000u;
	if (a->completes > 1 && ms > 0)
		kv_unum(kv_at("run", run, "consumed_fps"), (a->completed - a->first_bytes) / a->frame_bytes * 1000u / ms);
	return TRUE;
}

/**
 * audio_stop() - STOP (not draining), then count what is still reported consumed.
 * @t:   the tool
 * @a:   the run
 * @run: the run's number, for its keys (stop.N.*)
 *
 * Return: TRUE when STOP went out.
 */
static BOOL audio_stop(struct tool *t, struct audio_run *a, ULONG run)
{
	struct auds_msg m = {0};
	m.type = VCHIQ_LE32(AUDS_MSG_STOP);
	m.u.stop.draining = VCHIQ_LE32(0);
	if (audio_send(t, a, &m, "STOP") != 0)
		return FALSE;
	ULONG completed = a->completed, completes = a->completes;
	while (audio_take(t, a, AUDIO_STOP_QUIET_MS, NULL) >= 0)
		;
	kv_unum(kv_at("stop", run, "completed"), a->completed - completed);
	kv_unum(kv_at("stop", run, "queued"), a->sent - completed);
	kv_unum(kv_at("stop", run, "completes"), a->completes - completes);
	return TRUE;
}

/**
 * do_audio() - AUDIO: CONTROL and CONFIG, then the chunks with depth in flight, drained, STOP, CLOSE.
 * @t:    the tool
 * @args: audio_template's
 *
 * Return: RETURN_OK; RETURN_FAIL for arguments that make no sense or a protocol failure;
 * RETURN_ERROR without memory or vchiq.device.
 */
int do_audio(struct tool *t, LONG *args)
{
	struct audio_run a = {0};
	a.dest = arg_num(args, ARG_DEST, AUDS_DEST_HDMI0);
	a.rate = arg_num(args, ARG_RATE, 48000u);
	a.channels = arg_num(args, ARG_CHANNELS, 2u);
	a.chunk_ms = arg_num(args, ARG_CHUNK, 100u);
	a.depth = arg_num(args, ARG_DEPTH, 1u);
	a.volume = arg_num(args, ARG_VOL, AUDS_VOLUME_UNITY);
	a.walk = args[ARG_WALK] != 0;
	a.tone = args[ARG_TONE] != 0 || a.walk;
	ULONG ms = arg_num(args, ARG_MS, a.walk ? audio_walk_ms(a.channels) : 500u);
	BOOL restart = args[ARG_RESTART] != 0;
	if (a.channels == 0 || a.channels > 8)
	{
		kv_str("fail", "CHANNELS: 1 to 8");
		return RETURN_FAIL;
	}
	a.frame_bytes = a.channels * 2u;
	a.chunk_bytes = a.rate * a.chunk_ms / 1000u * a.frame_bytes;
	if (a.chunk_bytes == 0 || a.depth == 0)
	{
		kv_str("fail", "RATE, CHUNK and DEPTH must not be 0");
		return RETURN_FAIL;
	}
	kv_unum("audio.dest", a.dest);
	kv_unum("audio.rate", a.rate);
	kv_unum("audio.channels", a.channels);
	kv_unum("audio.chunk_ms", a.chunk_ms);
	kv_unum("audio.depth", a.depth);
	kv_num("audio.volume", (LONG)a.volume);
	kv_unum("audio.ms", ms);
	kv_str("audio.content", a.walk ? "walk" : a.tone ? "tone" : "silence");
	UBYTE *raw = AllocMem(a.chunk_bytes + 64u, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
	if (raw == NULL)
	{
		kv_str("fail", "no memory for a chunk");
		return RETURN_ERROR;
	}
	UBYTE *buf = (UBYTE *)(((ULONG)raw + 63u) & ~63UL);

	int rc = RETURN_FAIL;
	if (!vq_open(&t->svc, &t->waiter.timer, AUDS_SERVICE_FOURCC, AUDS_SERVICE_VERSION, AUDS_SERVICE_VERSION_MIN, NULL))
	{
		kv_num("vchiq.error", t->svc.error);
		kv_str("fail", "the AUDS service did not open");
		rc = t->svc.error == IOERR_OPENFAIL ? RETURN_ERROR : RETURN_FAIL;
		goto out_free;
	}
	kv_unum("audio.peer_version", t->svc.io->iovq_PeerVersion);
	ULONG pins = audio_pins();

	/* OPEN is not answered; a RESULT to it would be noted with the next wait */
	struct auds_msg m = {0};
	m.type = VCHIQ_LE32(AUDS_MSG_OPEN);
	if (audio_send(t, &a, &m, "OPEN") != 0)
		goto out_close;

	m = (struct auds_msg){0};
	m.type = VCHIQ_LE32(AUDS_MSG_CONTROL);
	m.u.control.volume = VCHIQ_LE32(a.volume);
	m.u.control.dest = VCHIQ_LE32(a.dest);
	if (!audio_request(t, &a, &m, "CONTROL", "control.result"))
		goto out_close;

	m = (struct auds_msg){0};
	m.type = VCHIQ_LE32(AUDS_MSG_CONFIG);
	m.u.config.channels = VCHIQ_LE32(a.channels);
	m.u.config.samplerate = VCHIQ_LE32(a.rate);
	m.u.config.bps = VCHIQ_LE32(16);
	if (!audio_request(t, &a, &m, "CONFIG", "config.result"))
		goto out_close;

	ULONG chunks = (ms + a.chunk_ms - 1u) / a.chunk_ms;
	if (a.walk)
		note("the walk: channel 1 beeps once, channel %lu %lu times, %lu ms apart\n", a.channels, a.channels, 2u * AUDIO_WALK_BEEP_MS);
	BOOL ok = audio_play(t, &a, buf, chunks, TRUE, 0) && audio_stop(t, &a, 0);
	kv_hex("pins.before", pins);
	kv_hex("pins.playing", a.pins_playing);
	if (ok && restart)
	{
		note("again, STOP with %lu chunks queued\n", a.depth);
		ok = audio_play(t, &a, buf, chunks, FALSE, 1) && audio_stop(t, &a, 1);
	}

	m = (struct auds_msg){0};
	m.type = VCHIQ_LE32(AUDS_MSG_CLOSE);
	if (audio_request(t, &a, &m, "CLOSE", "close.result") && ok)
		rc = RETURN_OK;
	kv_unum("audio.unexpected", a.unexpected);

out_close:
	vq_close(&t->svc);
out_free:
	FreeMem(raw, a.chunk_bytes + 64u);
	return rc;
}
