/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_audio.h - videocore.audio internals.
 *
 * An AHI sub-driver that plays through the VPU firmware's audio service
 * ("AUDS", auds_proto.h) on vchiq.device.  Two modes (16-bit and HiFi), each
 * with three outputs: HDMI0, HDMI1 and the headphone jack (AHIC_Output).
 * ahi.device mixes (AHISF_MIXING|AHISF_TIMING); the VPU's consumption paces
 * the passes.
 *
 * Every AHIsub_AllocAudio() gets a struct vca_stream and a slave task of its
 * own, which opens one AUDS service, owns every request and buffer, and calls
 * AHI's player and mixer hooks.  AHIsub_Start() and AHIsub_Stop() are
 * commands the slave carries out while the caller waits on the stream's
 * drv_waitq; AHIsub_Update() and AHIsub_HardwareControl() only store and
 * Signal(), so they are safe from interrupts.  The library base holds the
 * libraries and nothing else, so streams are independent of each other.
 */
#ifndef VIDEOCORE_AUDIO_H
#define VIDEOCORE_AUDIO_H

#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function, from base->SysBase */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/utility_protos.h>
#else
#include <proto/exec.h>
#include <proto/utility.h> /* calls go through a local UtilityBase, like SysBase */
#endif

#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <devices/ahi.h>
#include <libraries/ahi_sub.h>

#include <types.h>
#include <debug.h>
#include <drv_timer.h>
#include <drv_waitq.h>
#include <devices/vchiq.h>
#include <auds_proto.h>

#if defined(__INTELLISENSE__)
#define asm(x)
#define __attribute__(x)
#endif

#ifndef LIBRARY_NAME
#define LIBRARY_NAME "videocore.audio"
#endif
/* The version comes from CMake (the component's one version, root CMakeLists.txt); an editor's parser gets placeholders. */
#if !defined(LIBRARY_REVISION) || !defined(LIBRARY_IDSTRING)
#ifdef __INTELLISENSE__
#define LIBRARY_REVISION 0
#define LIBRARY_IDSTRING "$VER: videocore.audio (editor)"
#else
#error "LIBRARY_REVISION and LIBRARY_IDSTRING come from CMake"
#endif
#endif

/*
 * lib_Version is the AHI driver interface the library implements, not the
 * component's version: 4, so AHI 4 opens it as well as AHI 6 (which a V6
 * driver would need).  It obliges AHIsub_Disable() and AHIsub_Enable() to
 * preserve every register.
 */
#define LIBRARY_VERSION 4
#define LIBRARY_PRIORITY 0

#define VCA_TASK_STACK 16384
#define VCA_TASK_PRIORITY 10 /* above normal tasks and Executive's range; the slave never spins */

#define VCA_READS 4     /* reads kept posted: COMPLETEs come about every 10 ms */
#define VCA_CHUNKS 8    /* sample buffers, one WRITE and one bulk transmit each */
#define VCA_CHUNK_MS 10 /* one chunk's playing time */
#define VCA_TARGET_MS 40 /* what the slave keeps queued at the VPU */

#define VCA_REPLY_MS 2000    /* a RESULT, or requests coming home */
#define VCA_COMMAND_MS 10000 /* Start and Stop waiting for the slave */
#define VCA_TICK_MS 250      /* the stall check while playing */
#define VCA_STALL_TICKS 4    /* no COMPLETE for this many ticks with samples queued: the VPU is gone */

struct VCAudioBase
{
	struct Library lib;
	struct ExecBase *SysBase; /* right after lib: the Disable/Enable stubs read it at offset 34 */
	struct Library *UtilityBase;
	ULONG segList;
};

/* vca_request.kind */
enum vca_kind
{
	VCA_READ,    /* a CMD_READ: a message from the VPU lands in msg */
	VCA_CONTROL, /* a CMD_WRITE of OPEN, CONTROL, CONFIG, START, STOP or CLOSE, one at a time */
	VCA_WRITE,   /* a chunk's WRITE */
	VCA_BULK,    /* a chunk's samples */
};

/* A request to vchiq.device: a copy of the opened one. */
struct vca_request
{
	struct IOVchiqReq io;
	UBYTE kind;      /* enum vca_kind */
	BOOL at_device;  /* sent and not yet back */
	struct auds_msg msg; /* a read's message, or the one a write sends */
};

/* One buffer of samples on its way to the VPU */
struct vca_chunk
{
	struct vca_request write; /* the WRITE announcing it */
	struct vca_request bulk;  /* the samples */
	UBYTE *data;              /* little-endian 16-bit samples, channels to a frame, chunk_bytes */
};

/* AHIsub_Start() and AHIsub_Stop(), carried out by the slave */
enum vca_command
{
	VCA_CMD_START = 1,
	VCA_CMD_STOP,
};

/* One AHIsub_AllocAudio(): its slave, its AUDS service, its buffers (ahiac_DriverData). */
struct vca_stream
{
	struct VCAudioBase *base;
	struct AHIAudioCtrlDrv *audioctrl;
	struct Task *task; /* the slave, once ready; drv_task_join() waits for NULL */
	BYTE wake_sig;     /* the slave's: a command or a volume */

	ULONG rate;         /* ahiac_MixFreq */
	ULONG channels;     /* CONFIG's: 8 for a 7.1 mode, else 2 */
	ULONG vpu_frame;    /* bytes of a frame in a chunk: 16 bits a channel (mix_frame: AHI's) */
	ULONG chunk_bytes;  /* one chunk, whole frames */
	ULONG target_bytes; /* the queue kept at the VPU */

	/* AHIsub_Start(), AHIsub_Stop(): under lock; the callers wait on waitq */
	struct SignalSemaphore lock;
	struct drv_waitq waitq;
	ULONG cmd;        /* enum vca_command */
	ULONG cmd_seq;    /* the callers': bumped with each command */
	ULONG done_seq;   /* the slave's: the last command carried out */
	ULONG cmd_result; /* its AHIE_* */

	/* AHIsub_HardwareControl() stores these; the slave sends them as CONTROL */
	volatile LONG volume; /* AHIC_OutputVolume, Fixed */
	volatile ULONG dest;  /* AHIC_Output, as AUDS_DEST_* */

	/* the slave's alone from here */
	struct MsgPort *port; /* every request's replies */
	struct drv_timer timer;
	struct IOVchiqReq service; /* the opened request */
	struct vca_request reads[VCA_READS];
	struct vca_request control;
	struct vca_chunk chunks[VCA_CHUNKS];
	UBYTE *chunk_mem;
	ULONG outstanding; /* requests at the device */
	BOOL result_pending; /* a CONTROL, CONFIG or CLOSE waits for its RESULT */
	BOOL result_ok;      /* the last RESULT was a success */
	LONG volume_sent;  /* what the last CONTROL carried */
	ULONG dest_sent;
	BOOL closing;      /* reads are not posted again */
	BOOL dead;         /* the service failed: the timer paces the hooks, the samples are dropped */
	BOOL leaked;       /* a request never came back: the stream is never freed */

	/* playing: between a Start and a Stop */
	BOOL playing;
	BOOL started;   /* START sent */
	BOOL ticking;   /* the timer armed for the tick */
	ULONG buff_type; /* ahiac_BuffType: AHIST_S16S, AHIST_S32S or AHIST_L7_1 */
	ULONG mix_frame; /* bytes of a frame in mix: 16 or 32 bits a channel (vpu_frame: the VPU's) */
	APTR mix;       /* the mixer hook's buffer, ahiac_BuffSize */
	ULONG mix_size;
	ULONG pending;  /* frames of the last pass not yet in a chunk */
	ULONG mix_pos;  /* where they start in mix, in bytes */
	ULONG cur;      /* the chunk being filled */
	ULONG fill;     /* bytes in it */
	ULONG sent;     /* bytes sent since START */
	ULONG consumed; /* bytes the VPU reported consumed */
	ULONG underruns;
	ULONG stall_ticks;
	ULONG last_consumed;
};

/* audio_lib.c */
extern const char vca_idstring[];

/* audio_sub.c: the AHIsub_* entry points */
ULONG AHIsub_AllocAudio(struct TagItem *tags asm("a1"), struct AHIAudioCtrlDrv *audioctrl asm("a2"),
                        struct VCAudioBase *base asm("a6"));
void AHIsub_FreeAudio(struct AHIAudioCtrlDrv *audioctrl asm("a2"), struct VCAudioBase *base asm("a6"));
ULONG AHIsub_Start(ULONG flags asm("d0"), struct AHIAudioCtrlDrv *audioctrl asm("a2"), struct VCAudioBase *base asm("a6"));
void AHIsub_Update(ULONG flags asm("d0"), struct AHIAudioCtrlDrv *audioctrl asm("a2"), struct VCAudioBase *base asm("a6"));
void AHIsub_Stop(ULONG flags asm("d0"), struct AHIAudioCtrlDrv *audioctrl asm("a2"), struct VCAudioBase *base asm("a6"));
ULONG AHIsub_Unknown(void);
LONG AHIsub_GetAttr(ULONG attribute asm("d0"), LONG argument asm("d1"), LONG def asm("d2"), struct TagItem *tags asm("a1"),
                    struct AHIAudioCtrlDrv *audioctrl asm("a2"), struct VCAudioBase *base asm("a6"));
LONG AHIsub_HardwareControl(ULONG attribute asm("d0"), LONG argument asm("d1"), struct AHIAudioCtrlDrv *audioctrl asm("a2"),
                            struct VCAudioBase *base asm("a6"));

/* audio_slave.c */
void vca_slave(struct vca_stream *s, struct Task *parent);

#endif /* VIDEOCORE_AUDIO_H */
