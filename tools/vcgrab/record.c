/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * record.c - one recording: the screen through an encoder session into a
 * Matroska file.
 *
 * The session takes the screen as it is, and inside the VPU the ISP scales it
 * and hands it to the encoder, so a picture goes on without a copy and neither
 * side of that passes the host.  The screen's memory is the session's one
 * input frame.
 *
 * It goes out on a tick and comes back when the ISP has taken it.  A tick that
 * finds it still out is dropped, not queued; that also holds when the encoder
 * is behind, as the ISP waits for it.  A tick while the screen is not in front
 * is a pause.  Times come from the ticks' schedule, so a drop or a pause is a
 * gap in the file and the rest stays regular.
 *
 *   tick --> the screen, sent --> ISP ==tunnel==> encoder --> packets --> mkv --> the file
 *            the screen, back <--+                            a bounded write a pass
 *
 * The session tells the ISP's colours to the stream by itself: full-range
 * BT.601 of RGB, whatever its ports are told.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/videocodec_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/videocodec.h>
#endif

#include <dos/dos.h>
#include <exec/memory.h>
#include <string.h>

#include <timing.h>

#include "mkv.h"
#include "vcgrab.h"

#define PACKET_COUNT 4u                     /* output buffers the encoder writes into */
#define PACKET_BYTES (512u * 1024u)         /* the size of each; a picture too big for one, such as a 1 MB key frame, spans several */
#define SCREEN_TIMEOUT_US ((VCTime)2000000) /* the screen is out and nothing has come back from the VPU for this long: give up */
#define END_TIMEOUT_US ((VCTime)2000000)    /* how long a stop waits for the end mark to come out of the encoder */
#define WAIT_MS_MAX 100u                    /* the longest sleep in a pass: file writes and time checks have no signal to wake them */
#define MACROBLOCKS_PER_SEC_MAX 245760u     /* H.264 level 4.0, the encoder's */
#define FILE_BYTES_MAX (0xFFFFFFFFu - (64u << 20)) /* PFS3 ends at 4 GiB: stop short of it, as the last pictures and the Cues are still to come */

struct record
{
	/* Given to record_start() */
	struct waiter *waiter;
	const struct source *src;
	const struct record_params *params;

	/* The encoder */
	struct VCSession *encoder;
	ULONG output_width, output_height; /* the size encoded, which may differ from the screen's */
	struct VCFrame *screen_frame;      /* the screen's memory, imported as the session's one input frame */
	BOOL frame_in_flight;              /* sent and not back yet: a tick now is dropped */
	BOOL session_started;              /* a picture has been sent, which is what starts the session */

	/* Whether the encoder is still working, for time_check() */
	VCTime last_progress;        /* when a picture last went, or the frame or a packet last came back */
	ULONG pictures_since_packet; /* sent since a packet last came out */

	/* The file */
	BPTR file;
	APTR mkv_memory; /* the muxer's working memory, MKV_MEMORY bytes */
	struct mkv mkv;

	/* The clock: microseconds since the start, kept here as get_time() wraps at 71 minutes */
	u32 clock_last;     /* get_time() at the last clock_read() */
	VCTime now;         /* as of the last clock_read() */
	VCTime time_asleep; /* how much of now went on waiting; the rest is the report's 68k load */

	/* The ticks: they count from the start, params->fps a second */
	ULONG next_tick;
	ULONG ticks_dropped; /* due while the frame was in flight, or passed over by a stall */
	ULONG ticks_paused;  /* due while the screen was not in front */

	/* The end */
	BOOL stopping;       /* pictures are over and only the end mark is left */
	BOOL end_sent;       /* the end mark has gone to the session */
	BOOL end_seen;       /* it came out of the encoder, or nothing had been sent to end */
	VCTime end_deadline; /* when the wait for it is over */
	BOOL failed;         /* an error was said; the recording is over */
	BOOL reason_said;    /* it stopped by itself and told the window why: no summary over that */
};

static struct record record;

/* ---- the clock ---------------------------------------------------------------- */

/**
 * clock_read() - Bring the recording's clock up to now.
 * @r: the recording
 */
static void clock_read(struct record *r)
{
	u32 t = get_time();
	r->now += (VCTime)(u32)(t - r->clock_last);
	r->clock_last = t;
}

/**
 * tick_time() - Tell when a tick falls due.
 * @r:    the recording
 * @tick: the tick
 *
 * Worked out from the tick's number, not by adding intervals, so rounding
 * cannot build up over a long take.
 *
 * Rounded up because tick_take() goes the other way, from a time to a tick,
 * rounding down, and a time that has reached a tick must come back as that
 * tick.  At 30 fps tick 1 falls at 33333.3 us: rounded down to 33333, it would
 * come back as tick 0, and rounded up to 33334 it comes back as tick 1.
 *
 * Return: its time in microseconds.
 */
static VCTime tick_time(const struct record *r, ULONG tick)
{
	return DIV_CEIL((VCTime)tick * 1000000, (VCTime)r->params->fps);
}

/* ---- errors ----------------------------------------------------------------------- */

/**
 * fail() - End the recording on an error, saying the first.
 * @r:     the recording
 * @what:  what failed
 * @error: the error
 */
static void fail(struct record *r, const char *what, LONG error)
{
	if (!r->failed)
		grab_error(what, error);
	r->failed = TRUE;
}

/**
 * mux_fail() - End the recording on an error of the muxer's.
 * @r:     the recording
 * @error: what the muxer returned; MKV_OK does nothing
 */
static void mux_fail(struct record *r, int error)
{
	if (error == MKV_ERR_WRITE)
		fail(r, "the file took no more", IoErr());
	else if (error != MKV_OK)
		fail(r, "the encoder's stream went wrong", error);
}

/* ---- the file -------------------------------------------------------------------- */

/**
 * file_write() - Write for the muxer (struct mkv_io).
 * @user: the recording
 * @data: the bytes
 * @size: how many
 *
 * Return: nonzero when they all went.
 */
static int file_write(void *user, const void *data, uint32_t size)
{
	struct record *r = user;
	return Write(r->file, (APTR)data, (LONG)size) == (LONG)size;
}

/**
 * file_seek() - Seek for the muxer (struct mkv_io).
 * @user:     the recording
 * @position: where the next write goes
 *
 * From above 2 GiB Seek() answers a negative old position, so IoErr() tells.
 *
 * Return: nonzero when it will.
 */
static int file_seek(void *user, uint32_t position)
{
	struct record *r = user;
	SetIoErr(0);
	(void)Seek(r->file, (LONG)position, OFFSET_BEGINNING);
	return IoErr() == 0;
}

static const struct mkv_io file_io = {&record, file_write, file_seek};

/**
 * file_open() - Create the file and start the muxer on it.
 * @r: the recording
 *
 * Last of what record_start() does: a recording that is refused leaves a
 * file of that name as it was.
 *
 * Return: TRUE, FALSE with the reason said.
 */
static BOOL file_open(struct record *r)
{
	r->file = Open((CONST_STRPTR)r->params->file, MODE_NEWFILE);
	if (r->file == 0)
	{
		grab_fault(r->params->file);
		return FALSE;
	}
	const struct mkv_track track = {
		.codec = MKV_H264, .width = r->output_width, .height = r->output_height, .fps = r->params->fps, .app = "VCGrab"};
	mkv_open(&r->mkv, &file_io, &track, r->mkv_memory, MKV_MEMORY);
	return TRUE;
}

/* ---- the pictures --------------------------------------------------------------- */

/**
 * picture_send() - Send the screen to be a picture.
 * @r:   the recording
 * @pts: the picture's time
 */
static void picture_send(struct record *r, VCTime pts)
{
	r->screen_frame->Pts = pts;
	r->screen_frame->Flags = 0;
	LONG error = VC_SendFrame(r->encoder, r->screen_frame);
	if (error != 0)
	{
		fail(r, "the encoder refused the screen", error);
		return;
	}
	r->frame_in_flight = TRUE;
	r->session_started = TRUE;
	r->pictures_since_packet++;
	clock_read(r);
	r->last_progress = r->now; /* the first send starts the session and takes its time */
}

/**
 * end_mark() - Put the end mark on the way, as soon as that can be done.
 * @r: the recording, stopping
 *
 * The mark goes alone: the screen may be another's by now, so its pixels are
 * not read again.  With nothing sent there is nothing to end, and a mark would
 * only start the session.
 */
static void end_mark(struct record *r)
{
	if (r->end_sent || r->frame_in_flight)
		return;
	if (!r->session_started)
	{
		r->end_seen = TRUE;
		return;
	}
	r->screen_frame->Pts = tick_time(r, r->next_tick);
	r->screen_frame->Flags = VCFF_EndOfStream | VCFF_NoPicture;
	LONG error = VC_SendFrame(r->encoder, r->screen_frame);
	if (error != 0)
		fail(r, "the encoder refused the end mark", error);
	r->end_sent = TRUE;
}

/**
 * packet_take() - Give a packet to the muxer and release it.
 * @r:  the recording
 * @pk: the packet: a picture may come in several, its time and key flag with the first
 */
static void packet_take(struct record *r, struct VCPacket *pk)
{
	if (pk->Length != 0)
	{
		mkv_picture_begin(&r->mkv, (uint32_t)(pk->Pts / 1000), (pk->Flags & VCPF_KeyFrame) != 0);
		mkv_picture_bytes(&r->mkv, pk->Data, pk->Length);
		if ((pk->Flags & VCPF_FrameEnd) != 0)
			mkv_picture_end(&r->mkv);
	}
	VC_ReleasePacket(r->encoder, pk);
	mux_fail(r, r->mkv.error);
}

/**
 * session_item() - Act on what the session has: the screen back, a packet, the end, a failure.
 * @r:  the recording
 * @it: the item
 */
static void session_item(struct record *r, const struct VCItem *it)
{
	switch (it->Type)
	{
	case VCIT_FrameReturned:
		r->frame_in_flight = FALSE;
		r->last_progress = r->now;
		if (r->stopping)
			end_mark(r);
		break;
	case VCIT_Packet:
		r->pictures_since_packet = 0;
		r->last_progress = r->now;
		packet_take(r, it->u.Packet);
		break;
	case VCIT_Event:
		if (it->u.Event.Event == VCEV_EndOfStream)
			r->end_seen = TRUE;
		else if (it->u.Event.Event == VCEV_Error)
			fail(r, "the encoder failed", it->u.Event.Status);
		break;
	default:
		break;
	}
}

/**
 * drain() - Take everything the session has ready.
 * @r: the recording
 *
 * The clock first: what comes back now has come, however long the file took.
 */
static void drain(struct record *r)
{
	clock_read(r);
	struct VCItem it;
	while (VC_Poll(r->encoder, &it))
		session_item(r, &it);
}

/* ---- stopping ------------------------------------------------------------------- */

/**
 * record_stop() - Ask the recording to end: it goes on through record_pass() until record_over().
 * @r: the recording
 */
void record_stop(struct record *r)
{
	if (r->stopping)
		return;
	clock_read(r);
	r->stopping = TRUE;
	r->end_deadline = r->now + END_TIMEOUT_US;
	end_mark(r);
}

/**
 * stop_saying() - End the recording of its own accord, the reason being the last word on it.
 * @r:   the recording
 * @why: the reason
 */
static void stop_saying(struct record *r, const char *why)
{
	grab_say("%s", why);
	r->reason_said = TRUE;
	record_stop(r);
}

/**
 * record_over() - Tell whether only record_finish() is left.
 * @r: the recording
 *
 * Return: TRUE when the end mark is through, the time for it is up, or it failed.
 */
BOOL record_over(const struct record *r)
{
	return r->failed || (r->stopping && (r->end_seen || r->now >= r->end_deadline));
}

/* ---- a pass ------------------------------------------------------------------- */

/**
 * tick_take() - Act on the tick that is due, and count those a stall went past: only the latest is taken.
 * @r: the recording
 */
static void tick_take(struct record *r)
{
	ULONG latest = (ULONG)(r->now * (VCTime)r->params->fps / 1000000);
	ULONG skipped = latest - r->next_tick;
	r->next_tick = latest + 1u;
	switch (source_state(r->src))
	{
	case SOURCE_HIDDEN:
		r->ticks_paused += skipped + 1u;
		return;
	case SOURCE_MOVED:
		stop_saying(r, "the screen moved in memory, or flips between bitmaps: stopped");
		return;
	default:
		break;
	}
	r->ticks_dropped += skipped;
	if (r->frame_in_flight)
		r->ticks_dropped++;
	else
		picture_send(r, tick_time(r, latest));
}

/**
 * time_check() - Act on what time has brought: an end, pictures gone missing, a tick that is due.
 * @r: the recording, running, the session just drained
 */
static void time_check(struct record *r)
{
	ULONG seconds = r->params->seconds;
	if (seconds != 0 && r->now >= (VCTime)seconds * 1000000)
		record_stop(r);
	else if (mkv_bytes(&r->mkv) >= FILE_BYTES_MAX)
		stop_saying(r, "the file is nearly 4 GiB, as far as PFS3 goes: stopped");
	else if (r->frame_in_flight && r->now - r->last_progress > SCREEN_TIMEOUT_US)
		fail(r, "the screen did not come back", 0);
	else if (r->pictures_since_packet > r->params->fps)
		fail(r, "the encoder made nothing of a second of pictures", 0);
	else if (r->now >= tick_time(r, r->next_tick))
		tick_take(r);
}

/**
 * record_pass() - Do one round: what the file can take, what the session has, the tick if due, and a wait.
 * @r:     the recording
 * @extra: more signals to wake for
 *
 * The file goes first: a write can take its time, and what the session
 * finished meanwhile must be in before the clock judges it.
 *
 * Return: the signals seen, Ctrl-C among them.
 */
ULONG record_pass(struct record *r, ULONG extra)
{
	mux_fail(r, mkv_flush(&r->mkv, MKV_WRITE_MOST));
	drain(r);
	clock_read(r);
	if (!r->failed && !r->stopping)
		time_check(r);

	if (record_over(r))
		return 0;

	VCTime due = r->stopping ? r->end_deadline : tick_time(r, r->next_tick);
	ULONG wait_ms = due > r->now ? umin((u32)DIV_CEIL(due - r->now, (VCTime)1000), WAIT_MS_MAX) : 1u;

	VCTime sleep_start = r->now;
	ULONG seen = waiter_wait(r->waiter, extra, wait_ms);
	clock_read(r);
	r->time_asleep += r->now - sleep_start;
	return seen;
}

/* ---- the session ---------------------------------------------------------------- */

/**
 * encoder_limits() - Read the largest picture the H.264 encoder takes.
 * @width:  where the width goes
 * @height: where the height goes
 */
void encoder_limits(ULONG *width, ULONG *height)
{
	struct VCCodecInfo ci;
	*width = *height = 0;
	for (ULONG i = 0;; i++)
	{
		ci.Size = sizeof(ci);
		if (VC_QueryCodec(i, &ci) != 0)
			return;
		if (ci.Codec == VCC_H264 && ci.Kind == VCK_Encoder)
		{
			*width = ci.MaxWidth;
			*height = ci.MaxHeight;
			return;
		}
	}
}

/**
 * picture_size() - Settle the encoder's picture: as asked, else the screen's, even, made to fit.
 * @r: the recording
 *
 * Return: TRUE, FALSE with the reason said.
 */
static BOOL picture_size(struct record *r)
{
	ULONG max_w, max_h;
	encoder_limits(&max_w, &max_h);
	ULONG w = r->params->width, h = r->params->height;
	if (w > max_w || h > max_h)
	{
		grab_say("the encoder takes at most %lux%lu", max_w, max_h);
		return FALSE;
	}
	if (w == 0)
	{
		w = r->src->width;
		h = r->src->height;
		if (w * max_h > h * max_w && w > max_w)
		{
			h = h * max_w / w;
			w = max_w;
		}
		else if (h > max_h)
		{
			w = w * max_h / h;
			h = max_h;
		}
	}
	r->output_width = w & ~1u;
	r->output_height = h & ~1u;
	ULONG macroblocks = DIV_CEIL(r->output_width, 16u) * DIV_CEIL(r->output_height, 16u) * r->params->fps;
	if (macroblocks > MACROBLOCKS_PER_SEC_MAX)
	{
		grab_say("%lux%lu at %lu pictures a second is more than the encoder's level 4.0 takes", r->output_width, r->output_height, r->params->fps);
		return FALSE;
	}
	return TRUE;
}

/**
 * session_open() - Open the encoder, the screen as its input.
 * @r: the recording
 *
 * Return: TRUE, FALSE with the reason said.
 */
static BOOL session_open(struct record *r)
{
	const struct record_params *p = r->params;
	const struct source *src = r->src;
	LONG error = 0;
	r->encoder = VC_OpenSessionTags(VCA_Codec, VCC_H264, VCA_Kind, VCK_Encoder,
	                                VCA_Width, src->width, VCA_Height, src->height,
	                                VCA_InputFormat, src->format->vcf, VCA_InputPitch, src->pitch, VCA_InputCount, 1,
	                                VCA_OutputWidth, r->output_width, VCA_OutputHeight, r->output_height,
	                                VCA_FrameRateNum, p->fps, VCA_FrameRateDen, 1,
	                                VCA_Bitrate, p->bitrate, VCA_KeyInterval, p->fps * 2u,
	                                VCA_OutputCount, PACKET_COUNT, VCA_OutputSize, PACKET_BYTES,
	                                VCA_SigTask, (ULONG)r->waiter->task, VCA_SigMask, waiter_sigmask(r->waiter),
	                                VCA_ErrorPtr, (ULONG)&error,
	                                TAG_DONE);
	if (r->encoder == NULL)
	{
		/* picture_size() settled all else that could be wrong with these */
		if (error == VCERR_BADPARAM)
			grab_say("the VideoCore cannot read this screen's rows, %lu pixels wide", src->pitch / src->format->bpp);
		else
			fail(r, "the encoder did not open", error);
		return FALSE;
	}
	r->screen_frame = VC_ImportFrame(r->encoder, VCS_Input, src->memory);
	if (r->screen_frame == NULL)
		fail(r, "the encoder did not take the screen", 0);
	return r->screen_frame != NULL;
}

/**
 * session_close() - Close the session, if it opened.
 * @r: the recording
 */
static void session_close(struct record *r)
{
	if (r->encoder != NULL)
		VC_CloseSession(r->encoder);
}

/**
 * record_release() - Let go of the memory and the file; a file with no picture in it is deleted.
 * @r: the recording
 */
static void record_release(struct record *r)
{
	if (r->mkv_memory != NULL)
		FreeVec(r->mkv_memory);
	if (r->file == 0)
		return;
	Close(r->file);
	if (r->mkv.pictures == 0)
		DeleteFile((CONST_STRPTR)r->params->file);
}

/**
 * memory_free() - Read what the VPU has left, for the report: while the session still holds its own.
 *
 * Return: the bytes, 0 if it does not say.
 */
static ULONG memory_free(void)
{
	struct VCInfo info;
	memset(&info, 0, sizeof(info));
	info.Size = sizeof(info);
	return VC_GetBackendInfo(0, &info) == 0 ? info.MemFree : 0;
}

/**
 * record_start() - Open the session and the file, ready to take ticks.
 * @waiter: the task's signal and timer
 * @src:    the screen, taken until record_finish()
 * @params: what to record, valid until record_finish()
 *
 * Return: the recording, NULL if it did not start: nothing is held, and the reason was said.
 */
struct record *record_start(struct waiter *waiter, const struct source *src, const struct record_params *params)
{
	struct record *r = &record;
	memset(r, 0, sizeof(*r));
	r->waiter = waiter;
	r->src = src;
	r->params = params;
	if (!picture_size(r))
		return NULL;
	r->mkv_memory = AllocVec(MKV_MEMORY, MEMF_PUBLIC);
	if (r->mkv_memory == NULL)
	{
		grab_say("no memory");
		return NULL;
	}
	if (!session_open(r) || !file_open(r))
	{
		session_close(r);
		record_release(r);
		return NULL;
	}
	r->clock_last = get_time();
	return r;
}

/**
 * record_finish() - Finish the file, then close the session and report.
 * @r: the recording, over
 *
 * The file first: the session's ports can take seconds to close, and what
 * happens there must not cost the recording its end.
 *
 * Return: RETURN_OK, RETURN_FAIL if it failed on the way.
 */
int record_finish(struct record *r)
{
	clock_read(r);
	ULONG mem = memory_free();
	mux_fail(r, mkv_close(&r->mkv));
	record_release(r);
	session_close(r);
	ULONG busy = r->now != 0 ? (ULONG)((r->now - r->time_asleep) * 100 / r->now) : 0;
	ULONG ms = (ULONG)(r->now / 1000);
	const char *warning = r->stopping && !r->end_seen ? ", no end mark back" : "";
	grab_report("%lu pictures, %lux%lu, %lu ms, %lu ticks dropped, %lu paused, %lu bytes, 68k busy %lu%%, %lu MiB free on the VPU%s",
	            r->mkv.pictures, r->output_width, r->output_height, ms, r->ticks_dropped, r->ticks_paused, r->mkv.written, busy, mem >> 20, warning);
	if (!r->failed && !r->reason_said)
		window_status("%s: %lu pictures, %lu ms, %lu ticks dropped%s", (const char *)FilePart((CONST_STRPTR)r->params->file), r->mkv.pictures, ms,
		              r->ticks_dropped, warning);
	return r->failed ? RETURN_FAIL : RETURN_OK;
}
