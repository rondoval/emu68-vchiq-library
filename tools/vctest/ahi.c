/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * ahi.c - the AHI side (ahi.device 6, the low-level API, so the mode is
 * chosen here and not by the user's unit prefs): AHIWALK sounds the walk in
 * a mode's channels (8 in AHI's 7.1 order for a multichannel mode, else 2);
 * AHITONE plays the tone through a mode for a while, timing the start and
 * the stop.  Through videocore.audio's modes these drive that driver.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/ahi_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ahi.h>
#endif

#include <dos/dos.h>
#include <exec/memory.h>
#include <devices/ahi.h>

#include <args.h>
#include <kv.h>
#include <timing.h>

#include "commands.h"
#include "tone.h"

const char ahiwalk_template[] = "MODE/N/A,RATE/N";
const char ahitone_template[] = "MODE/N/A,MS/N,RATE/N,HZ/N";

enum
{
	WALK_MODE,
	WALK_RATE
};

enum
{
	TONE_MODE,
	TONE_MS,
	TONE_RATE,
	TONE_HZ_ARG
};

#define WALK_BEEP_MS 200  /* one beep, and the quiet after it */
#define WALK_GAP_MS 1000  /* between two channels */
#define WALK_LEAD_MS 2000 /* silence first, while the display or receiver takes up the new format */

struct Library *AHIBase; /* the device's base, while it is open */

/* An AHI mode taken for playing */
struct ahi_mode
{
	struct MsgPort *port;
	struct AHIRequest *io;
	struct AHIAudioCtrl *ctrl;
	ULONG mode, rate, channels;
	BOOL multi;
	char name[64];
};

/**
 * ahi_open() - Open ahi.device 6 and allocate a mode with one channel and @sounds sounds.
 * @m:      the mode, zeroed
 * @mode:   the AHI mode id
 * @rate:   the mix rate
 * @sounds: AHIA_Sounds
 *
 * Return: RETURN_OK; RETURN_ERROR when the device did not open; RETURN_FAIL for no such mode or no audio.
 */
static int ahi_open(struct ahi_mode *m, ULONG mode, ULONG rate, ULONG sounds)
{
	m->mode = mode;
	m->rate = rate;
	m->port = CreateMsgPort();
	m->io = m->port != NULL ? (struct AHIRequest *)CreateIORequest(m->port, sizeof(*m->io)) : NULL;
	if (m->io == NULL)
	{
		kv_str("fail", "no memory for the AHI request");
		return RETURN_ERROR;
	}
	m->io->ahir_Version = 6; /* 32-bit and 7.1 samples */
	if (OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)m->io, 0) != 0)
	{
		kv_str("fail", "ahi.device 6 did not open");
		return RETURN_ERROR;
	}
	AHIBase = (struct Library *)m->io->ahir_Std.io_Device;
	ULONG multi = FALSE;
	if (!AHI_GetAudioAttrs(mode, NULL, AHIDB_Name, (ULONG)m->name, AHIDB_BufferLen, sizeof(m->name), AHIDB_MultiChannel, (ULONG)&multi,
	                       TAG_DONE))
	{
		kv_str("fail", "no such AHI mode");
		return RETURN_FAIL;
	}
	m->multi = multi != 0;
	m->channels = m->multi ? 8u : 2u;
	kv_hex("ahi.mode", mode);
	kv_str("ahi.name", m->name);
	kv_unum("ahi.channels", m->channels);
	kv_unum("ahi.rate", rate);
	m->ctrl = AHI_AllocAudio(AHIA_AudioID, mode, AHIA_MixFreq, rate, AHIA_Channels, 1, AHIA_Sounds, sounds, TAG_DONE);
	if (m->ctrl == NULL)
	{
		kv_str("fail", "AHI_AllocAudio failed");
		return RETURN_FAIL;
	}
	return RETURN_OK;
}

/**
 * ahi_close() - Free the audio and close the device.
 * @m: the mode, open or half open
 */
static void ahi_close(struct ahi_mode *m)
{
	if (m->ctrl != NULL)
		AHI_FreeAudio(m->ctrl);
	m->ctrl = NULL;
	if (AHIBase != NULL)
		CloseDevice((struct IORequest *)m->io);
	AHIBase = NULL;
	if (m->io != NULL)
		DeleteIORequest((struct IORequest *)m->io);
	m->io = NULL;
	if (m->port != NULL)
		DeleteMsgPort(m->port);
	m->port = NULL;
}

/**
 * ahi_beep() - The walk's beep as one sample of whole frames, the tone in each frame's last channel only.
 * @m:      the mode
 * @frames: the beep's length
 *
 * Sound c starts that many channels before the end of the first frame, so
 * the tone is its channel c (the trick of AHI's Test-7.1 example): one
 * buffer serves every channel.
 *
 * Return: the samples (FreeMem() (frames + 1) * channels * 4), NULL without memory.
 */
static LONG *ahi_beep(const struct ahi_mode *m, ULONG frames)
{
	LONG *beep = AllocMem((frames + 1u) * m->channels * 4u, MEMF_PUBLIC | MEMF_CLEAR);
	ULONG phase = 0;
	if (beep != NULL)
		for (ULONG i = 0; i < frames; i++)
			beep[(i + 1u) * m->channels - 1u] = (LONG)tone_sample(&phase, m->rate, TONE_HZ) * 0x10000; /* 32-bit, the 16 bits on top */
	return beep;
}

/**
 * ahi_walk() - The walk through a playing audio control: the lead, then channel c's beep c + 1 times and a gap each.
 * @m: the mode, playing
 *
 * Return: TRUE unless Ctrl-C.
 */
static BOOL ahi_walk(const struct ahi_mode *m)
{
	Delay(WALK_LEAD_MS / 20u); /* ticks of 20 ms */
	for (ULONG c = 0; c < m->channels; c++)
	{
		for (ULONG b = 0; b <= c; b++)
		{
			AHI_Play(m->ctrl, AHIP_BeginChannel, 0, AHIP_Freq, m->rate, AHIP_Vol, 0x10000, AHIP_Pan, 0x8000, AHIP_Sound, c, AHIP_LoopSound,
			         (ULONG)AHI_NOSOUND, AHIP_EndChannel, (ULONG)NULL, TAG_DONE);
			Delay(2u * WALK_BEEP_MS / 20u);
		}
		Delay(WALK_GAP_MS / 20u);
		if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
			return FALSE;
	}
	return TRUE;
}

/**
 * do_ahiwalk() - AHIWALK: the walk's sounds loaded into a mode, then played.
 * @t:    the tool
 * @args: ahiwalk_template's
 *
 * Return: RETURN_OK; RETURN_ERROR without ahi.device; RETURN_FAIL when the mode, the sounds or Play failed.
 */
int do_ahiwalk(struct tool *t, LONG *args)
{
	(void)t;
	struct ahi_mode m = {0};
	ULONG rate = arg_num(args, WALK_RATE, 48000u);
	int rc = ahi_open(&m, arg_num(args, WALK_MODE, 0), rate, 8);
	if (rc != RETURN_OK)
		goto out;
	rc = RETURN_FAIL;
	ULONG frames = rate * WALK_BEEP_MS / 1000u;
	LONG *beep = ahi_beep(&m, frames);
	if (beep == NULL)
	{
		kv_str("fail", "no memory for the beep");
		rc = RETURN_ERROR;
		goto out;
	}
	BOOL ok = TRUE;
	for (ULONG c = 0; c < m.channels && ok; c++)
	{
		struct AHISampleInfo si = {m.multi ? AHIST_L7_1 : AHIST_S32S, beep + (m.channels - 1u - c), frames};
		ok = AHI_LoadSound((UWORD)c, AHIST_SAMPLE, &si, m.ctrl) == AHIE_OK;
	}
	if (!ok)
		kv_str("fail", "AHI_LoadSound failed");
	else if (AHI_ControlAudio(m.ctrl, AHIC_Play, TRUE, TAG_DONE) != AHIE_OK)
		kv_str("fail", "AHIC_Play failed");
	else
	{
		note("the walk: channel 1 beeps once, channel %lu %lu times, %lu ms apart\n", m.channels, m.channels, 2u * WALK_BEEP_MS);
		ok = ahi_walk(&m);
		(void)AHI_ControlAudio(m.ctrl, AHIC_Play, FALSE, TAG_DONE);
		kv_bool("ahi.stopped", !ok);
		rc = RETURN_OK;
	}
	FreeMem(beep, (frames + 1u) * m.channels * 4u);
out:
	ahi_close(&m);
	return rc;
}

/**
 * do_ahitone() - AHITONE: the tone through a mode for MS ms, the start and the stop timed.
 * @t:    the tool
 * @args: ahitone_template's
 *
 * One looping stereo sound of whole periods; Ctrl-C ends it early.  ahi.us is
 * the time played, ahi.stop_us the time from stopping to the audio freed.
 *
 * Return: RETURN_OK; RETURN_ERROR without ahi.device or memory; RETURN_FAIL when the mode or Play failed.
 */
int do_ahitone(struct tool *t, LONG *args)
{
	struct ahi_mode m = {0};
	ULONG rate = arg_num(args, TONE_RATE, 48000u), ms = arg_num(args, TONE_MS, 2000u), hz = arg_num(args, TONE_HZ_ARG, TONE_HZ);
	if (hz < 20 || hz > rate / 2u)
	{
		kv_str("fail", "HZ: 20 to half the rate");
		return RETURN_FAIL;
	}
	int rc = ahi_open(&m, arg_num(args, TONE_MODE, 0), rate, 1);
	if (rc != RETURN_OK)
		goto out;
	rc = RETURN_FAIL;
	ULONG periods = hz / 5u, frames = periods * rate / hz; /* about 200 ms of whole periods: the loop seam is silent */
	WORD *tone = AllocMem(frames * 4u, MEMF_PUBLIC);
	if (tone == NULL)
	{
		kv_str("fail", "no memory for the tone");
		rc = RETURN_ERROR;
		goto out;
	}
	ULONG phase = 0;
	for (ULONG i = 0; i < frames; i++)
		tone[2 * i] = tone[2 * i + 1] = tone_sample(&phase, rate, hz);
	kv_unum("ahi.hz", hz);
	kv_unum("ahi.ms", ms);
	struct AHISampleInfo si = {AHIST_S16S, tone, frames};
	if (AHI_LoadSound(0, AHIST_SAMPLE, &si, m.ctrl) != AHIE_OK)
		kv_str("fail", "AHI_LoadSound failed");
	else if (AHI_ControlAudio(m.ctrl, AHIC_Play, TRUE, TAG_DONE) != AHIE_OK)
		kv_str("fail", "AHIC_Play failed");
	else
	{
		u32 t0 = get_time();
		AHI_Play(m.ctrl, AHIP_BeginChannel, 0, AHIP_Freq, rate, AHIP_Vol, 0x10000, AHIP_Pan, 0x8000, AHIP_Sound, 0, AHIP_LoopSound, 0,
		         AHIP_EndChannel, (ULONG)NULL, TAG_DONE);
		ULONG seen = waiter_wait(&t->waiter, 0, ms);
		u32 t1 = get_time();
		(void)AHI_ControlAudio(m.ctrl, AHIC_Play, FALSE, TAG_DONE);
		AHI_FreeAudio(m.ctrl);
		m.ctrl = NULL;
		u32 t2 = get_time();
		kv_unum("ahi.us", t1 - t0);
		kv_unum("ahi.stop_us", t2 - t1);
		kv_bool("ahi.stopped", (seen & SIGBREAKF_CTRL_C) != 0);
		rc = RETURN_OK;
	}
	FreeMem(tone, frames * 4u);
out:
	ahi_close(&m);
	return rc;
}
