/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * audio_sub.c - the AHIsub_* entry points.
 *
 * AllocAudio and FreeAudio start and join a stream's slave; Start and Stop
 * hand it a command and wait; Update and HardwareControl store and Signal()
 * (safe from interrupts).  The sound calls are ahi.device's: it mixes.
 */
#include <exec/memory.h>

#include <bits.h>
#include <driver_task.h>

#include <videocore_audio.h>

/* What every destination plays: the rates CONFIG takes (AHIDB_Frequency, AHIDB_Index) */
static const ULONG frequencies[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000};
#define FREQUENCIES ARRAY_SIZE(frequencies)

/* The outputs (AHIDB_Output, AHIC_Output), the default first */
static const struct
{
	ULONG dest; /* AUDS_DEST_* */
	const char *name;
} outputs[] = {
    {AUDS_DEST_HDMI0, "HDMI0"},
    {AUDS_DEST_HDMI1, "HDMI1"},
    {AUDS_DEST_HEADPHONES, "Headphone jack"},
};
#define OUTPUTS ARRAY_SIZE(outputs)

/* What GetAttr answers alike for every mode, whatever the argument */
static const struct
{
	ULONG attribute; /* AHIDB_* */
	LONG value;      /* a string's address for the strings */
} constant_attributes[] = {
    {AHIDB_Bits, 16},
    {AHIDB_Frequencies, FREQUENCIES},
    {AHIDB_Author, (LONG) "emu68-vchiq-library contributors"},
    {AHIDB_Copyright, (LONG) "\xa9 2026 emu68-vchiq-library contributors, BSD-3-Clause"},
    {AHIDB_Version, (LONG)(vca_idstring + 6)}, /* past "$VER: " */
    {AHIDB_Annotation, (LONG) "Plays through the VideoCore firmware's audio service\n"
                              "(vchiq.device): HDMI0, HDMI1 or the headphone jack."},
    {AHIDB_Record, FALSE},
    {AHIDB_FullDuplex, FALSE},
    {AHIDB_Realtime, TRUE}, /* the player hook runs at its rate, paced by what the VPU plays */
    {AHIDB_MinOutputVolume, 0},
    {AHIDB_MaxOutputVolume, 0x10000},
    {AHIDB_Outputs, OUTPUTS},
};

/**
 * index_or_0() - An index from a caller, or 0 when it is out of range.
 * @index: the index
 * @count: how many there are
 *
 * Return: @index if below @count, else 0.
 */
static ULONG index_or_0(LONG index, ULONG count)
{
	return (ULONG)index < count ? (ULONG)index : 0;
}

/**
 * wake_slave() - Signal a stream's slave, if it runs, to look at its commands and settings.
 * @base: the library base
 * @s:    the stream
 *
 * Safe from interrupts.
 */
static void wake_slave(struct VCAudioBase *base, struct vca_stream *s)
{
	struct ExecBase *SysBase = base->SysBase;
	if (s->task != NULL)
		Signal(s->task, 1UL << s->wake_sig);
}

/**
 * nearest_frequency() - The index of the supported rate closest to one asked for.
 * @rate: the rate, Hz
 *
 * Return: an index into frequencies[].
 */
static ULONG nearest_frequency(ULONG rate)
{
	ULONG best = 0, best_distance = ~0UL;
	for (ULONG i = 0; i < FREQUENCIES; i++)
	{
		ULONG distance = rate > frequencies[i] ? rate - frequencies[i] : frequencies[i] - rate;
		if (distance < best_distance)
		{
			best = i;
			best_distance = distance;
		}
	}
	return best;
}

/**
 * AHIsub_AllocAudio() - Set a stream up for one AHI_AllocAudio(): its slave and AUDS service.
 * @tags:      the mode's tags
 * @audioctrl: the audio control; ahiac_MixFreq is moved to the nearest rate supported
 * @base:      the library base (A6)
 *
 * A 7.1 mode (AHIACF_MULTICHANNEL, from its tags) plays 8 channels, the
 * others 2.  The slave is ready once the service is open and has taken
 * CONTROL (the default output) and CONFIG, so a machine without the VPU's
 * audio fails here.  AHIsub_FreeAudio() cleans up after a failure.
 *
 * Return: AHISF_MIXING | AHISF_TIMING | AHISF_KNOWSTEREO | AHISF_KNOWHIFI |
 * AHISF_KNOWMULTICHANNEL, or AHISF_ERROR.
 */
ULONG AHIsub_AllocAudio(struct TagItem *tags asm("a1"), struct AHIAudioCtrlDrv *audioctrl asm("a2"),
                        struct VCAudioBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;

	(void)tags;
	audioctrl->ahiac_MixFreq = frequencies[nearest_frequency(audioctrl->ahiac_MixFreq)];

	struct vca_stream *s = AllocMem(sizeof(*s), MEMF_PUBLIC | MEMF_CLEAR);
	audioctrl->ahiac_DriverData = s;
	if (s == NULL)
		return AHISF_ERROR;
	s->base = base;
	s->audioctrl = audioctrl;
	s->dest = outputs[0].dest;
	s->rate = audioctrl->ahiac_MixFreq;
	s->channels = audioctrl->ahiac_Flags & AHIACF_MULTICHANNEL ? 8u : 2u;
	s->vpu_frame = s->channels * 2u;
	s->chunk_bytes = s->rate * VCA_CHUNK_MS / 1000u * s->vpu_frame;
	s->target_bytes = s->rate * VCA_TARGET_MS / 1000u * s->vpu_frame;
	s->volume = 0x10000;
	s->wake_sig = -1;
	InitSemaphore(&s->lock);
	drv_waitq_init(&s->waitq);

	if (drv_task_spawn(SysBase, s, (APTR)vca_slave, LIBRARY_NAME, VCA_TASK_STACK, VCA_TASK_PRIORITY) != 0)
		return AHISF_ERROR;
	return AHISF_MIXING | AHISF_TIMING | AHISF_KNOWSTEREO | AHISF_KNOWHIFI | AHISF_KNOWMULTICHANNEL;
}

/**
 * AHIsub_FreeAudio() - End a stream: its slave stops playing, closes the service and exits.
 * @audioctrl: the audio control
 * @base:      the library base (A6)
 *
 * Safe after a failed AHIsub_AllocAudio() and when called twice.  A stream
 * the device still holds a request of is left allocated.
 */
void AHIsub_FreeAudio(struct AHIAudioCtrlDrv *audioctrl asm("a2"), struct VCAudioBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	struct vca_stream *s = audioctrl->ahiac_DriverData;
	if (s == NULL)
		return;
	drv_task_join(SysBase, &s->task);
	drv_waitq_cleanup(&s->waitq, SysBase);
	if (!s->leaked)
		FreeMem(s, sizeof(*s));
	audioctrl->ahiac_DriverData = NULL;
}

/**
 * command_done() - Whether the slave has carried out a command.
 * @a: the stream, its lock held
 * @b: the command's sequence number
 *
 * Return: TRUE once done_seq has reached it.
 */
static BOOL command_done(APTR a, APTR b)
{
	const struct vca_stream *s = a;
	return (LONG)(s->done_seq - (ULONG)b) >= 0;
}

/**
 * command() - Hand the slave a command and wait until it has carried it out.
 * @base: the library base
 * @s:    the stream
 * @cmd:  enum vca_command
 *
 * A later command overtaking this one answers for both.
 *
 * Return: its AHIE_*, AHIE_UNKNOWN without a slave or when the wait failed.
 */
static ULONG command(struct VCAudioBase *base, struct vca_stream *s, ULONG cmd)
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&s->lock);
	if (s->task == NULL)
	{
		ReleaseSemaphore(&s->lock);
		return AHIE_UNKNOWN;
	}
	ULONG seq = ++s->cmd_seq;
	s->cmd = cmd;
	wake_slave(base, s);
	LONG r = drv_wait_event(&s->waitq, SysBase, &s->lock, command_done, s, (APTR)seq, VCA_COMMAND_MS);
	ULONG result = r == DRV_WAIT_OK ? s->cmd_result : AHIE_UNKNOWN;
	ReleaseSemaphore(&s->lock);
	if (r != DRV_WAIT_OK)
		Kprintf("[videocore.audio] command %ld: the slave did not answer (%ld)\n", cmd, r);
	return result;
}

/**
 * AHIsub_Start() - Start playing: the slave mixes ahead to its target and sends START.
 * @flags:     AHISF_PLAY; AHISF_RECORD is refused
 * @audioctrl: the audio control
 * @base:      the library base (A6)
 *
 * A Start while playing starts over.
 *
 * Return: AHIE_OK, AHIE_NOMEM, or AHIE_UNKNOWN (recording, no slave, a mixer buffer it cannot take).
 */
ULONG AHIsub_Start(ULONG flags asm("d0"), struct AHIAudioCtrlDrv *audioctrl asm("a2"), struct VCAudioBase *base asm("a6"))
{
	if (flags & AHISF_RECORD)
		return AHIE_UNKNOWN;
	if (!(flags & AHISF_PLAY) || audioctrl->ahiac_DriverData == NULL)
		return AHIE_OK;
	return command(base, audioctrl->ahiac_DriverData, VCA_CMD_START);
}

/**
 * AHIsub_Update() - Nothing to do: the slave rereads the hooks and ahiac_BuffSamples every pass.
 * @flags:     none defined
 * @audioctrl: the audio control
 * @base:      the library base (A6)
 */
void AHIsub_Update(ULONG flags asm("d0"), struct AHIAudioCtrlDrv *audioctrl asm("a2"), struct VCAudioBase *base asm("a6"))
{
	(void)flags;
	(void)audioctrl;
	(void)base;
}

/**
 * AHIsub_Stop() - Stop playing: no hook runs once it returns, the queued samples are dropped.
 * @flags:     AHISF_PLAY
 * @audioctrl: the audio control
 * @base:      the library base (A6)
 *
 * Safe without a Start, after a failed one and when called twice.
 */
void AHIsub_Stop(ULONG flags asm("d0"), struct AHIAudioCtrlDrv *audioctrl asm("a2"), struct VCAudioBase *base asm("a6"))
{
	if ((flags & AHISF_PLAY) && audioctrl->ahiac_DriverData != NULL)
		(void)command(base, audioctrl->ahiac_DriverData, VCA_CMD_STOP);
}

/**
 * AHIsub_Unknown() - AHIsub_SetVol() to AHIsub_UnloadSound(): ahi.device does them, it mixes.
 *
 * Return: AHIS_UNKNOWN.
 */
ULONG AHIsub_Unknown(void)
{
	return AHIS_UNKNOWN;
}

/**
 * mix_rate() - The rate a stream will mix at, for GetAttr.
 * @audioctrl: the audio control to come, or NULL
 *
 * Return: its ahiac_MixFreq, the highest rate without one.
 */
static ULONG mix_rate(const struct AHIAudioCtrlDrv *audioctrl)
{
	return audioctrl != NULL ? audioctrl->ahiac_MixFreq : frequencies[FREQUENCIES - 1];
}

/**
 * AHIsub_GetAttr() - Answer a question about a mode or the driver.
 * @attribute: AHIDB_*
 * @argument:  the index or rate some attributes take
 * @def:       the answer to anything not known here
 * @tags:      the mode's tags, or NULL
 * @audioctrl: an audio control to come, or NULL
 * @base:      the library base (A6)
 *
 * Return: the attribute's value, a string's address for the strings.
 */
LONG AHIsub_GetAttr(ULONG attribute asm("d0"), LONG argument asm("d1"), LONG def asm("d2"), struct TagItem *tags asm("a1"),
                    struct AHIAudioCtrlDrv *audioctrl asm("a2"), struct VCAudioBase *base asm("a6"))
{
	(void)tags; /* every mode answers alike */
	(void)base;
	for (ULONG i = 0; i < ARRAY_SIZE(constant_attributes); i++)
		if (constant_attributes[i].attribute == attribute)
			return constant_attributes[i].value;
	switch (attribute)
	{
	case AHIDB_Frequency:
		return (LONG)frequencies[index_or_0(argument, FREQUENCIES)];
	case AHIDB_Index:
		return (LONG)nearest_frequency((ULONG)argument);
	case AHIDB_MaxPlaySamples:
		return def + (LONG)(mix_rate(audioctrl) * VCA_TARGET_MS / 1000u);
	case AHIDB_Output:
		return (LONG)outputs[index_or_0(argument, OUTPUTS)].name;
	default:
		return def;
	}
}

/**
 * output_volume() - An AHIC_OutputVolume value, limited to the range GetAttr reports.
 * @volume: the value asked for, Fixed
 *
 * Return: 0 to 0x10000.
 */
static LONG output_volume(LONG volume)
{
	if (volume < 0)
		return 0;
	return volume > 0x10000 ? 0x10000 : volume;
}

/**
 * output_index() - Which output (AHIDB_Output) plays to a destination.
 * @dest: AUDS_DEST_*
 *
 * Return: its index, 0 (the default) for none.
 */
static ULONG output_index(ULONG dest)
{
	for (ULONG i = 0; i < OUTPUTS; i++)
		if (outputs[i].dest == dest)
			return i;
	return 0;
}

/**
 * AHIsub_HardwareControl() - Set or ask the output and its volume; no inputs.
 * @attribute: AHIC_*
 * @argument:  the value to set
 * @audioctrl: the audio control
 * @base:      the library base (A6)
 *
 * Safe from interrupts: a new volume or output is stored and the slave
 * signalled, which sends it as CONTROL, also while playing.
 *
 * Return: the value asked for, TRUE for a setting taken, FALSE for anything else.
 */
LONG AHIsub_HardwareControl(ULONG attribute asm("d0"), LONG argument asm("d1"), struct AHIAudioCtrlDrv *audioctrl asm("a2"),
                            struct VCAudioBase *base asm("a6"))
{
	struct vca_stream *s = audioctrl->ahiac_DriverData;
	if (s == NULL)
		return FALSE;

	switch (attribute)
	{
	case AHIC_OutputVolume:
		s->volume = output_volume(argument);
		wake_slave(base, s);
		return TRUE;
	case AHIC_OutputVolume_Query:
		return s->volume;
	case AHIC_Output:
		if ((ULONG)argument >= OUTPUTS)
			return FALSE;
		s->dest = outputs[argument].dest;
		wake_slave(base, s);
		return TRUE;
	case AHIC_Output_Query:
		return (LONG)output_index(s->dest);
	case AHIC_MixFreq_Query:
		return (LONG)audioctrl->ahiac_MixFreq;
	default:
		return FALSE;
	}
}
