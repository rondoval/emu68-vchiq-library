/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * audio_slave.c - a stream's slave task: the AUDS service, the passes, the pacing.
 *
 * The slave opens a unit of vchiq.device on the "AUDS" service, keeps reads
 * posted, and sends OPEN, CONTROL (volume, output) and CONFIG (2 or 8
 * channels, 16 bits, the mix rate) before it reports ready.  Every request
 * replies to its port; one Wait() loop handles them with the commands, the
 * volume and output, and the timer.
 *
 * Playing: a pass calls ahi.device's player hook, then its mixer hook into
 * mix; the frames are turned into little-endian 16-bit samples in the chunk
 * being filled (a 7.1 frame's channels reordered for the VPU), and a full
 * chunk goes out as a WRITE and its bulk transmit, back to back (the device
 * keeps a service's writes and bulk transmits in order).  COMPLETE reports
 * the bytes the VPU consumed.  Passes are mixed
 * while what is queued (sent - consumed, plus the chunk being filled) is
 * below target_bytes and a chunk is free; START follows the first passes.
 *
 * A service that fails (a request's error, or no COMPLETE for a second with
 * samples queued) makes the stream dead: the timer then paces the passes,
 * which are mixed and dropped, so the program's player keeps running.
 *
 * The file goes bottom-up: requests, the timer, replies, waits, CONTROL,
 * passes, commands, then the service's and the task's life.
 */
#include <exec/memory.h>
#include <exec/errors.h>
#include <dos/dos.h> /* SIGBREAKF_CTRL_C, _F: drv_task_spawn()'s and drv_task_join()'s signals */
#include <utility/hooks.h>

#include <bits.h>
#include <memory.h>
#include <driver_task.h>

#include <videocore_audio.h>

/* ---- requests ------------------------------------------------------------------------- */

/**
 * send() - Send a request to the device.
 * @s:       the stream
 * @r:       the request, home
 * @command: CMD_READ, CMD_WRITE or NSCMD_VQ_BULKWRITE
 * @data:    its buffer
 * @length:  its length
 */
static void send(struct vca_stream *s, struct vca_request *r, UWORD command, APTR data, ULONG length)
{
	struct ExecBase *SysBase = s->base->SysBase;
	r->io.iovq_Req.io_Command = command;
	r->io.iovq_Req.io_Data = data;
	r->io.iovq_Req.io_Length = length;
	r->at_device = TRUE;
	s->outstanding++;
	SendIO((struct IORequest *)&r->io);
}

/**
 * abort_request() - Abort a request if the device has it.
 * @s: the stream
 * @r: the request
 */
static void abort_request(struct vca_stream *s, struct vca_request *r)
{
	struct ExecBase *SysBase = s->base->SysBase;
	if (r->at_device)
		AbortIO((struct IORequest *)&r->io);
}

/**
 * msg_init() - Clear a message and set its type.
 * @m:    the message
 * @type: AUDS_MSG_*
 *
 * Return: @m, for the caller to fill in.
 */
static struct auds_msg *msg_init(struct auds_msg *m, ULONG type)
{
	memset(m, 0, sizeof(*m));
	m->type = VCHIQ_LE32(type);
	return m;
}

/**
 * control_msg() - The control request's message, cleared, of one type.
 * @s:    the stream, its control request home
 * @type: AUDS_MSG_*
 *
 * Return: the message, for the caller to fill in and control_send().
 */
static struct auds_msg *control_msg(struct vca_stream *s, ULONG type)
{
	return msg_init(&s->control.msg, type);
}

/**
 * control_send() - Send the message control_msg() gave.
 * @s: the stream
 */
static void control_send(struct vca_stream *s)
{
	send(s, &s->control, CMD_WRITE, &s->control.msg, sizeof(s->control.msg));
}

/**
 * chunk_busy() - Whether the device still has a chunk's WRITE or samples.
 * @c: the chunk
 *
 * Return: TRUE while either is out.
 */
static BOOL chunk_busy(const struct vca_chunk *c)
{
	return c->write.at_device || c->bulk.at_device;
}

/* ---- the timer -------------------------------------------------------------------------- */

/**
 * pass_ms() - How long one pass plays.
 * @s: the stream, playing
 *
 * Return: milliseconds, at least 1.
 */
static ULONG pass_ms(const struct vca_stream *s)
{
	ULONG ms = s->audioctrl->ahiac_BuffSamples * 1000u / s->rate;
	return ms > 0 ? ms : 1;
}

/**
 * arm_tick() - Arm the timer while playing: the stall check, or a dead stream's pass.
 * @s: the stream
 */
static void arm_tick(struct vca_stream *s)
{
	if (!s->playing || s->ticking)
		return;
	drv_timer_arm_ms(&s->timer, s->dead ? pass_ms(s) : VCA_TICK_MS);
	s->ticking = TRUE;
}

/**
 * stop_tick() - Cancel the timer if it is armed for the tick.
 * @s: the stream
 */
static void stop_tick(struct vca_stream *s)
{
	if (!s->ticking)
		return;
	drv_timer_cancel(&s->timer);
	s->ticking = FALSE;
}

/* ---- replies ---------------------------------------------------------------------------- */

/**
 * go_dead() - Give the service up: from now on the timer paces the passes.
 * @s:     the stream
 * @what:  what failed, for the log
 * @error: its error
 */
static void go_dead(struct vca_stream *s, const char *what, LONG error)
{
	(void)what; /* only the debug log reads them */
	(void)error;
	if (s->dead)
		return;
	s->dead = TRUE;
	Kprintf("[videocore.audio] %s (%ld): the stream goes on without the VPU\n", what, error);
	stop_tick(s); /* re-armed at a pass's period */
	arm_tick(s);
}

/**
 * message() - Take in a message from the VPU: COMPLETE's bytes, a RESULT.
 * @s: the stream
 * @m: the message
 */
static void message(struct vca_stream *s, const struct auds_msg *m)
{
	switch (VCHIQ_LE32(m->type))
	{
	case AUDS_MSG_COMPLETE:
	{
		ULONG count = VCHIQ_LE32(m->u.complete.count);
		s->consumed += count & AUDS_COMPLETE_BYTES;
		if (count & AUDS_COMPLETE_UNDERRUN)
			s->underruns++;
		break;
	}
	case AUDS_MSG_RESULT:
		s->result_pending = FALSE;
		s->result_ok = m->u.result.success == 0;
		if (!s->result_ok)
			Kprintf("[videocore.audio] a RESULT of %ld\n", (LONG)VCHIQ_LE32((ULONG)m->u.result.success));
		break;
	default:
		Kprintf("[videocore.audio] a message of type %ld\n", VCHIQ_LE32(m->type));
		break;
	}
}

/**
 * reply() - Handle a request back from the device: a read is posted again.
 * @s: the stream
 * @r: the request
 */
static void reply(struct vca_stream *s, struct vca_request *r)
{
	BYTE error = r->io.iovq_Req.io_Error;
	r->at_device = FALSE;
	s->outstanding--;
	if (r->kind == VCA_READ && (error == 0 || error == VQERR_TRUNCATED))
	{
		message(s, &r->msg);
		if (!s->closing)
			send(s, r, CMD_READ, &r->msg, sizeof(r->msg));
	}
	else if (error != 0 && error != IOERR_ABORTED)
		go_dead(s, r->kind == VCA_READ ? "a read failed" : "a write failed", error);
}

/**
 * handle_replies() - Handle every request on the port.
 * @s: the stream
 */
static void handle_replies(struct vca_stream *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct Message *msg;
	while ((msg = GetMsg(s->port)) != NULL)
		reply(s, (struct vca_request *)msg);
}

/* ---- waits ------------------------------------------------------------------------------ */

typedef BOOL (*vca_pred_t)(const struct vca_stream *s);

/**
 * control_home() - Whether the control request is home.
 * @s: the stream
 *
 * Return: TRUE when it is.
 */
static BOOL control_home(const struct vca_stream *s)
{
	return !s->control.at_device;
}

/**
 * chunks_home() - Whether every chunk is home.
 * @s: the stream
 *
 * Return: TRUE when none is busy.
 */
static BOOL chunks_home(const struct vca_stream *s)
{
	for (ULONG i = 0; i < VCA_CHUNKS; i++)
		if (chunk_busy(&s->chunks[i]))
			return FALSE;
	return TRUE;
}

/**
 * all_home() - Whether every request is home.
 * @s: the stream
 *
 * Return: TRUE when the device holds none.
 */
static BOOL all_home(const struct vca_stream *s)
{
	return s->outstanding == 0;
}

/**
 * result_in() - Whether the RESULT waited for came, or the service is gone.
 * @s: the stream
 *
 * Return: TRUE when nothing waits any more.
 */
static BOOL result_in(const struct vca_stream *s)
{
	return !s->result_pending || s->dead;
}

/**
 * wait_until() - Handle replies until a condition holds or a timeout passes.
 * @s:    the stream, not ticking (the timer takes the timeout)
 * @pred: the condition
 * @ms:   the timeout
 *
 * Commands and Ctrl-C stay pending for the loop.
 *
 * Return: whether the condition holds.
 */
static BOOL wait_until(struct vca_stream *s, vca_pred_t pred, ULONG ms)
{
	struct ExecBase *SysBase = s->base->SysBase;
	if (pred(s))
		return TRUE;
	ULONG port_mask = 1UL << s->port->mp_SigBit;
	ULONG timer_mask = drv_timer_sigmask(&s->timer);
	drv_timer_arm_ms(&s->timer, ms);
	BOOL ok;
	for (;;)
	{
		ULONG signals = Wait(port_mask | timer_mask);
		handle_replies(s);
		ok = pred(s);
		if (ok || (signals & timer_mask) != 0)
			break;
	}
	drv_timer_cancel(&s->timer);
	return ok;
}

/**
 * send_and_wait() - Send the control request's message and wait for the RESULT it gets.
 * @s:    the stream, its control request home
 * @what: the message's name, for the log
 *
 * Return: TRUE when a successful RESULT came.
 */
static BOOL send_and_wait(struct vca_stream *s, const char *what)
{
	(void)what; /* only the debug log reads it */
	s->result_pending = TRUE;
	s->result_ok = FALSE;
	control_send(s);
	if (!wait_until(s, result_in, VCA_REPLY_MS) || !s->result_ok)
	{
		Kprintf("[videocore.audio] %s: no RESULT, or a failed one\n", what);
		return FALSE;
	}
	return TRUE;
}

/* ---- volume and output ------------------------------------------------------------------ */

/* log2(1 + i/16) for i = 0..16, 16.16 */
static const ULONG log2_table[17] = {
    0,     5732,  11136, 16248, 21098, 25711, 30109, 34312, 38336,
    42196, 45904, 49472, 52911, 56229, 59434, 62534, 65536,
};

/**
 * attenuation() - CONTROL's volume for an AHI output volume.
 * @volume: AHIC_OutputVolume, Fixed, 0 to 0x10000
 *
 * 20 * log10(0x10000 / volume) dB in 1/256 dB, from log2 (one step of 6.0206
 * dB is 1541.3 units) with the fraction interpolated from a table.
 *
 * Return: the attenuation, AUDS_VOLUME_MUTE for 0.
 */
static LONG attenuation(LONG volume)
{
	if (volume <= 0)
		return AUDS_VOLUME_MUTE;
	if (volume >= 0x10000)
		return AUDS_VOLUME_UNITY;
	ULONG v = (ULONG)volume;
	ULONG msb = 31u - (ULONG)__builtin_clz(v);    /* 0 to 15 */
	ULONG mant = (v << (31u - msb)) & 0x7fffffffu; /* the fraction below the top bit, 31 bits */
	ULONG i = mant >> 27, f = (mant >> 11) & 0xffffu;
	ULONG frac = log2_table[i] + (((log2_table[i + 1] - log2_table[i]) * f) >> 16);
	ULONG log2v = (msb << 16) + frac;                  /* 16.16 */
	ULONG att = (((16u << 16) - log2v) * 1541u) >> 16; /* 1541.3 per octave */
	return att > AUDS_VOLUME_MUTE ? AUDS_VOLUME_MUTE : (LONG)att;
}

/**
 * control_fill() - CONTROL for the volume and output AHIsub_HardwareControl() stored.
 * @s: the stream, its control request home
 */
static void control_fill(struct vca_stream *s)
{
	struct auds_msg *m = control_msg(s, AUDS_MSG_CONTROL);
	s->volume_sent = s->volume;
	s->dest_sent = s->dest;
	m->u.control.volume = VCHIQ_LE32((ULONG)attenuation(s->volume_sent));
	m->u.control.dest = VCHIQ_LE32(s->dest_sent);
}

/**
 * update_control() - Send CONTROL if the volume or the output changed and the control request is free.
 * @s: the stream
 *
 * Also while playing.
 */
static void update_control(struct vca_stream *s)
{
	if (s->dead || s->control.at_device || (s->volume == s->volume_sent && s->dest == s->dest_sent))
		return;
	control_fill(s);
	control_send(s);
}

/* ---- passes ----------------------------------------------------------------------------- */

/**
 * streaming() - Whether the stream plays to the VPU: playing, the service alive.
 * @s: the stream
 *
 * Return: TRUE when passes go to the VPU.
 */
static BOOL streaming(const struct vca_stream *s)
{
	return s->playing && !s->dead;
}

/**
 * queued() - The bytes sent or in the chunk being filled and not yet consumed.
 * @s: the stream
 *
 * Return: the count, 0 when a COMPLETE of samples dropped by a STOP made consumed overtake sent.
 */
static ULONG queued(const struct vca_stream *s)
{
	LONG q = (LONG)(s->sent + s->fill - s->consumed);
	return q > 0 ? (ULONG)q : 0;
}

/**
 * mix_pass() - One pass: ahi.device's player hook, then its mixer hook into mix.
 * @s: the stream, playing
 *
 * The player may change ahiac_BuffSamples (AHIsub_Update()), so it is read
 * after; it never exceeds what ahiac_BuffSize holds.
 */
static void mix_pass(struct vca_stream *s)
{
	struct Library *UtilityBase = s->base->UtilityBase;
	struct AHIAudioCtrlDrv *audioctrl = s->audioctrl;

	if (audioctrl->ahiac_PlayerFunc != NULL)
		CallHookPkt(audioctrl->ahiac_PlayerFunc, audioctrl, NULL);
	ULONG frames = umin(audioctrl->ahiac_BuffSamples, s->mix_size / s->mix_frame);
	CallHookPkt(audioctrl->ahiac_MixerFunc, audioctrl, s->mix);
	s->pending = frames;
	s->mix_pos = 0;
}

/*
 * The VPU's order of a 7.1 frame, each channel by its place in AHI's (front
 * left, front right, rear left, rear right, side left, side right, centre,
 * LFE): HDMI's 7.1 (CEA-861) front left, front right, LFE, centre, rear left,
 * rear right, rear left centre, rear right centre.  AHI's rear pair is a 5.1
 * program's surround pair, which a 5.1 receiver takes as rear left and right.
 * Heard so far: the front pair (a stereo display drops the rest).
 */
static const UBYTE l71_from[8] = {0, 1, 7, 6, 2, 3, 4, 5};

/**
 * swap16() - Two big-endian 16-bit samples in a long, made little-endian.
 * @x: the long
 *
 * Return: @x with each word's bytes swapped.
 */
static ULONG swap16(ULONG x)
{
	return ((x & 0x00ff00ffu) << 8) | ((x >> 8) & 0x00ff00ffu);
}

/**
 * convert() - Mixed frames to little-endian 16-bit samples in the VPU's order.
 * @s:      the stream: its buffer type
 * @dst:    where they go, long-aligned
 * @src:    the mixer's frames, big-endian
 * @frames: how many
 *
 * AHIST_S32S's and AHIST_L7_1's upper words are the 16-bit samples.
 */
static void convert(const struct vca_stream *s, ULONG *dst, const ULONG *src, ULONG frames)
{
	switch (s->buff_type)
	{
	case AHIST_S16S:
		for (ULONG i = 0; i < frames; i++)
			dst[i] = swap16(src[i]);
		break;
	case AHIST_S32S:
		for (ULONG i = 0; i < frames; i++, src += 2)
			dst[i] = swap16((src[0] & 0xffff0000u) | (src[1] >> 16));
		break;
	default: /* AHIST_L7_1 */
		for (ULONG i = 0; i < frames; i++, src += 8)
			for (ULONG c = 0; c < 8; c += 2)
				*dst++ = swap16((src[l71_from[c]] & 0xffff0000u) | (src[l71_from[c + 1]] >> 16));
		break;
	}
}

/**
 * send_chunk() - Send the chunk being filled: its WRITE, then its samples.
 * @s: the stream
 */
static void send_chunk(struct vca_stream *s)
{
	struct vca_chunk *c = &s->chunks[s->cur];
	struct auds_msg *m = msg_init(&c->write.msg, AUDS_MSG_WRITE);
	m->u.write.count = VCHIQ_LE32(s->fill);
	m->u.write.cookie1 = VCHIQ_LE32(AUDS_WRITE_COOKIE1);
	m->u.write.cookie2 = VCHIQ_LE32(AUDS_WRITE_COOKIE2);
	send(s, &c->write, CMD_WRITE, m, sizeof(*m));
	send(s, &c->bulk, NSCMD_VQ_BULKWRITE, c->data, s->fill);
	s->sent += s->fill;
	s->fill = 0;
	s->cur = (s->cur + 1u) % VCA_CHUNKS;
}

/**
 * fill_chunk() - Move the pass's frames into the chunk being filled; send it when full.
 * @s: the stream, with frames pending
 *
 * Return: FALSE when that chunk is still out (the ring is full).
 */
static BOOL fill_chunk(struct vca_stream *s)
{
	struct vca_chunk *c = &s->chunks[s->cur];
	if (chunk_busy(c))
		return FALSE;
	ULONG frames = umin(s->pending, (s->chunk_bytes - s->fill) / s->vpu_frame);
	convert(s, (ULONG *)(c->data + s->fill), (const ULONG *)((UBYTE *)s->mix + s->mix_pos), frames);
	s->fill += frames * s->vpu_frame;
	s->mix_pos += frames * s->mix_frame;
	s->pending -= frames;
	if (s->fill == s->chunk_bytes)
		send_chunk(s);
	return TRUE;
}

/**
 * send_start() - Send START once the first samples are out and the control request is free.
 * @s: the stream
 */
static void send_start(struct vca_stream *s)
{
	if (!streaming(s) || s->started || s->sent == 0 || s->control.at_device)
		return;
	(void)control_msg(s, AUDS_MSG_START);
	control_send(s);
	s->started = TRUE;
}

/**
 * pump() - Mix and send while the queue is short and a chunk is free; START once primed.
 * @s: the stream
 */
static void pump(struct vca_stream *s)
{
	while (streaming(s))
	{
		if (s->pending > 0)
		{
			if (!fill_chunk(s))
				break; /* the ring is full: a chunk coming home calls again */
		}
		else if (queued(s) < s->target_bytes)
			mix_pass(s);
		else
			break;
	}
	send_start(s);
}

/**
 * tick() - The timer while playing: a dead stream's pass, or the stall check.
 * @s: the stream
 */
static void tick(struct vca_stream *s)
{
	drv_timer_consume(&s->timer);
	s->ticking = FALSE;
	if (s->dead)
	{
		mix_pass(s);
		s->pending = 0; /* dropped */
	}
	else if (s->started)
	{
		if (s->consumed == s->last_consumed && queued(s) > 0)
		{
			if (++s->stall_ticks >= VCA_STALL_TICKS)
				go_dead(s, "no COMPLETE", 0);
		}
		else
			s->stall_ticks = 0;
		s->last_consumed = s->consumed;
	}
	arm_tick(s);
}

/* ---- commands --------------------------------------------------------------------------- */

/**
 * send_stop() - STOP, which drops what the VPU queued, once anything was sent.
 * @s: the stream
 *
 * Samples sent before START would play at the next START: STOP drops them too.
 */
static void send_stop(struct vca_stream *s)
{
	if (s->sent == 0 || s->dead || !wait_until(s, control_home, VCA_REPLY_MS))
		return;
	struct auds_msg *m = control_msg(s, AUDS_MSG_STOP);
	m->u.stop.draining = 0;
	control_send(s);
}

/**
 * collect_chunks() - Wait for every chunk; abort what the device still holds.
 * @s: the stream
 *
 * A chunk that still does not come back makes the stream dead.
 */
static void collect_chunks(struct vca_stream *s)
{
	if (wait_until(s, chunks_home, VCA_REPLY_MS))
		return;
	for (ULONG i = 0; i < VCA_CHUNKS; i++)
	{
		abort_request(s, &s->chunks[i].write);
		abort_request(s, &s->chunks[i].bulk);
	}
	if (!wait_until(s, chunks_home, VCA_REPLY_MS))
		go_dead(s, "chunks stuck at the device", 0);
}

/**
 * do_stop() - Stop playing: no more passes, STOP, every chunk home, the mixer's buffer freed.
 * @s: the stream
 */
static void do_stop(struct vca_stream *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	if (!s->playing)
		return;
	s->playing = FALSE;
	stop_tick(s);
	send_stop(s);
	collect_chunks(s);
	Kprintf("[videocore.audio] stop: %ld bytes sent, %ld consumed, %ld underruns\n", s->sent, s->consumed, s->underruns);
	FreeMem(s->mix, s->mix_size);
	s->mix = NULL;
}

/**
 * mix_frame_bytes() - The size of a mixed frame, for a buffer type the stream takes.
 * @s:    the stream: 2 channels take AHIST_S16S and AHIST_S32S, 8 AHIST_L7_1
 * @type: ahiac_BuffType
 *
 * Return: bytes, 0 for a type it does not take.
 */
static ULONG mix_frame_bytes(const struct vca_stream *s, ULONG type)
{
	if (s->channels == 8)
		return type == AHIST_L7_1 ? 32u : 0u;
	if (type == AHIST_S16S)
		return 4u;
	return type == AHIST_S32S ? 8u : 0u;
}

/**
 * do_start() - Start playing; pump() then primes the queue and sends START.
 * @s: the stream
 *
 * Return: AHIE_OK, AHIE_NOMEM, or AHIE_UNKNOWN for a mixer buffer it does not take.
 */
static ULONG do_start(struct vca_stream *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	struct AHIAudioCtrlDrv *audioctrl = s->audioctrl;

	do_stop(s);
	ULONG mix_frame = mix_frame_bytes(s, audioctrl->ahiac_BuffType);
	if (mix_frame == 0)
		return AHIE_UNKNOWN;
	s->mix_size = audioctrl->ahiac_BuffSize;
	s->mix = AllocMem(s->mix_size, MEMF_PUBLIC);
	if (s->mix == NULL)
		return AHIE_NOMEM;
	s->buff_type = audioctrl->ahiac_BuffType;
	s->mix_frame = mix_frame;
	s->pending = 0;
	s->fill = 0;
	s->sent = 0;
	s->consumed = 0;
	s->underruns = 0;
	s->stall_ticks = 0;
	s->last_consumed = 0;
	s->started = FALSE;
	s->playing = TRUE;
	arm_tick(s);
	Kprintf("[videocore.audio] start: %ld Hz, %ld channels to %ld, %ld frames a pass of %ld bytes each\n", s->rate, s->channels,
	        s->dest, audioctrl->ahiac_BuffSamples, mix_frame);
	return AHIE_OK;
}

/**
 * take_command() - Carry out the command a caller is waiting for, if there is one.
 * @s: the stream
 */
static void take_command(struct vca_stream *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	ObtainSemaphore(&s->lock);
	ULONG seq = s->cmd_seq, cmd = s->cmd;
	ReleaseSemaphore(&s->lock);
	if (seq == s->done_seq)
		return;

	ULONG result = AHIE_OK;
	if (cmd == VCA_CMD_START)
		result = do_start(s);
	else
		do_stop(s);

	ObtainSemaphore(&s->lock);
	s->cmd_result = result;
	s->done_seq = seq;
	drv_wake_up_all(&s->waitq, SysBase);
	ReleaseSemaphore(&s->lock);
}

/* ---- the service ------------------------------------------------------------------------ */

/**
 * chunk_stride() - The distance between two chunks: chunk_bytes, rounded to 64 bytes.
 * @s: the stream
 *
 * Return: bytes.
 */
static ULONG chunk_stride(const struct vca_stream *s)
{
	return ALIGN_UP(s->chunk_bytes, 64u);
}

/**
 * init_request() - Make a request a copy of the opened one: its device, unit and reply port.
 * @s:    the stream, its service open
 * @r:    the request
 * @kind: enum vca_kind
 */
static void init_request(struct vca_stream *s, struct vca_request *r, UBYTE kind)
{
	struct ExecBase *SysBase = s->base->SysBase;
	CopyMem(&s->service, &r->io, sizeof(s->service));
	r->kind = kind;
}

/**
 * open_service() - The chunks, the AUDS service, its requests; the reads posted.
 * @s: the stream, its port and timer ready
 *
 * Return: TRUE when the service is open.
 */
static BOOL open_service(struct vca_stream *s)
{
	struct ExecBase *SysBase = s->base->SysBase;

	s->chunk_mem = AllocMem(VCA_CHUNKS * chunk_stride(s), MEMF_FAST | MEMF_PUBLIC);
	if (s->chunk_mem == NULL)
		return FALSE;

	struct IOVchiqReq *service = &s->service;
	service->iovq_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
	service->iovq_Req.io_Message.mn_ReplyPort = s->port;
	service->iovq_Req.io_Message.mn_Length = sizeof(*service);
	service->iovq_FourCC = AUDS_SERVICE_FOURCC;
	service->iovq_Version = AUDS_SERVICE_VERSION;
	service->iovq_VersionMin = AUDS_SERVICE_VERSION_MIN;
	if (OpenDevice((CONST_STRPTR)VCHIQ_NAME, 0, (struct IORequest *)service, 0) != 0)
	{
		Kprintf("[videocore.audio] %s: the AUDS service did not open (%ld)\n", VCHIQ_NAME, (LONG)service->iovq_Req.io_Error);
		service->iovq_Req.io_Device = NULL;
		return FALSE;
	}
	Kprintf("[videocore.audio] AUDS open, peer version %ld\n", (ULONG)service->iovq_PeerVersion);

	init_request(s, &s->control, VCA_CONTROL);
	for (ULONG i = 0; i < VCA_CHUNKS; i++)
	{
		init_request(s, &s->chunks[i].write, VCA_WRITE);
		init_request(s, &s->chunks[i].bulk, VCA_BULK);
		s->chunks[i].data = s->chunk_mem + i * chunk_stride(s);
	}
	for (ULONG i = 0; i < VCA_READS; i++)
	{
		init_request(s, &s->reads[i], VCA_READ);
		send(s, &s->reads[i], CMD_READ, &s->reads[i].msg, sizeof(s->reads[i].msg));
	}
	return TRUE;
}

/**
 * configure_service() - OPEN, then CONTROL (volume, output) and CONFIG, each answered.
 * @s: the stream, its service open
 *
 * Return: TRUE when the VPU took all three.
 */
static BOOL configure_service(struct vca_stream *s)
{
	/* OPEN gets no answer: its write coming home is enough */
	(void)control_msg(s, AUDS_MSG_OPEN);
	control_send(s);
	if (!wait_until(s, control_home, VCA_REPLY_MS))
		return FALSE;

	control_fill(s);
	if (!send_and_wait(s, "CONTROL") || !wait_until(s, control_home, VCA_REPLY_MS))
		return FALSE;

	struct auds_msg *m = control_msg(s, AUDS_MSG_CONFIG);
	m->u.config.channels = VCHIQ_LE32(s->channels);
	m->u.config.samplerate = VCHIQ_LE32(s->rate);
	m->u.config.bps = VCHIQ_LE32(16);
	return send_and_wait(s, "CONFIG") && !s->dead;
}

/**
 * close_service() - CLOSE, collect every request, close the device.
 * @s: the stream, its service open
 *
 * A request that never comes back leaves the port (made silent) and the
 * stream allocated (leaked): the device may still write to them.
 */
static void close_service(struct vca_stream *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	if (!s->dead && wait_until(s, control_home, VCA_REPLY_MS))
	{
		(void)control_msg(s, AUDS_MSG_CLOSE);
		(void)send_and_wait(s, "CLOSE");
	}
	s->closing = TRUE;
	for (ULONG i = 0; i < VCA_READS; i++)
		abort_request(s, &s->reads[i]);
	if (!wait_until(s, all_home, VCA_REPLY_MS))
		Kprintf("[videocore.audio] %ld requests still at %s at the close\n", s->outstanding, VCHIQ_NAME);
	CloseDevice((struct IORequest *)&s->service);
	if (wait_until(s, all_home, VCA_REPLY_MS))
		return;
	Kprintf("[videocore.audio] %ld requests never came back: the stream stays allocated\n", s->outstanding);
	s->port->mp_Flags = PA_IGNORE;
	s->leaked = TRUE;
}

/* ---- the task --------------------------------------------------------------------------- */

/**
 * slave_init() - The signal, the port and the timer; then the service, open and configured.
 * @s: the stream
 *
 * Return: TRUE when the stream can play.
 */
static BOOL slave_init(struct vca_stream *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	s->wake_sig = AllocSignal(-1);
	s->port = CreateMsgPort();
	BOOL timer = drv_timer_open(&s->timer, SysBase);
	return s->wake_sig >= 0 && s->port != NULL && timer && open_service(s) && configure_service(s);
}

/**
 * slave_cleanup() - Stop, close the service, free what slave_init() made (what no request still uses).
 * @s: the stream, after slave_init(), whatever it returned
 */
static void slave_cleanup(struct vca_stream *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	if (s->port == NULL)
		return; /* nothing beyond the signal, which goes with the task */
	do_stop(s);
	if (s->service.iovq_Req.io_Device != NULL)
		close_service(s);
	drv_timer_close(&s->timer);
	if (s->leaked)
		return; /* the device may still write to the port and the chunks */
	if (s->chunk_mem != NULL)
		FreeMem(s->chunk_mem, VCA_CHUNKS * chunk_stride(s));
	s->chunk_mem = NULL;
	DeleteMsgPort(s->port);
}

/**
 * slave_loop() - Handle replies, the timer, commands and CONTROL, and mix, until Ctrl-C.
 * @s: the stream, configured
 */
static void slave_loop(struct vca_stream *s)
{
	struct ExecBase *SysBase = s->base->SysBase;
	ULONG port_mask = 1UL << s->port->mp_SigBit;
	ULONG wake_mask = 1UL << s->wake_sig;
	ULONG timer_mask = drv_timer_sigmask(&s->timer);
	for (;;)
	{
		ULONG signals = Wait(port_mask | wake_mask | timer_mask | SIGBREAKF_CTRL_C);
		if (signals & SIGBREAKF_CTRL_C)
			return;
		handle_replies(s);
		/* CheckIO(): the replies may have re-armed it, go_dead() does, and the signal was the old request's */
		if ((signals & timer_mask) && s->ticking && CheckIO((struct IORequest *)s->timer.req))
			tick(s);
		if (signals & wake_mask)
			take_command(s);
		update_control(s);
		pump(s);
	}
}

/**
 * vca_slave() - A stream's slave task (drv_task_spawn()).
 * @s:      the stream
 * @parent: the AHIsub_AllocAudio() caller, told when the service is configured or failed
 *
 * Runs until drv_task_join()'s Ctrl-C (AHIsub_FreeAudio()).
 */
void vca_slave(struct vca_stream *s, struct Task *parent)
{
	struct ExecBase *SysBase = s->base->SysBase;
	if (!slave_init(s))
	{
		slave_cleanup(s);
		drv_task_exit(SysBase, &s->task, parent, FALSE);
		return;
	}
	s->task = FindTask(NULL);
	Signal(parent, SIGBREAKF_CTRL_F);
	slave_loop(s);
	slave_cleanup(s);
	/* the AllocAudio caller may be long gone: report to ourselves, the join polls the slot */
	drv_task_exit(SysBase, &s->task, FindTask(NULL), TRUE);
}
