/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * encode.c - ENCODE: a raw file (ffmpeg's rawvideo) through an H.264 encoder
 * session; KEYREQ asks for a key frame before picture n.  FORMAT and OUTWIDTH,
 * OUTHEIGHT take it through the ISP.  GAPS leaves gaps in the times, and every
 * picture's time is held against what was sent.  ENDEMPTY ends with a bare
 * mark.  HOLD keeps the packets after the nth, so that the encoder stalls, and
 * closes the session like that.  PACE sends at FPS, as a recorder does, where
 * ENCODE otherwise sends as fast as frames come back.
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

#include <args.h>
#include <kv.h>
#include <timing.h>
#include <vc_names.h>

#include "commands.h"
#include "picture.h"

const char encode_template[] =
    "FILE/A,WIDTH/N/A,HEIGHT/N/A,OUT/A,FPS/N,BITRATE/N,KEYINT/N,KEYREQ/N,BACKEND/K,FORMAT/K,OUTWIDTH/K/N,OUTHEIGHT/K/N,HOLD/K/N,GAPS/S,ENDEMPTY/S,PACE/S,PACKET/K/N";

#define HOLD_MS 1000u  /* with HOLD, nothing moving for this long is the stall asked for */
#define GAP_EVERY 10u  /* GAPS: pictures between two gaps */
#define GAP_US 200000u /* GAPS: how long each is */
#define FRAME_LATE_US 10000u /* PACE: a picture this far behind its time counts as late */

enum
{
	ARG_FILE,
	ARG_WIDTH,
	ARG_HEIGHT,
	ARG_OUT,
	ARG_FPS,
	ARG_BITRATE,
	ARG_KEYINT,
	ARG_KEYREQ,
	ARG_BACKEND,
	ARG_FORMAT,
	ARG_OUTWIDTH,
	ARG_OUTHEIGHT,
	ARG_HOLD,
	ARG_GAPS,
	ARG_ENDEMPTY,
	ARG_PACE,
	ARG_PACKET
};

/* One ENCODE run */
struct encode
{
	struct tool *t;
	struct VCSession *s;
	BPTR file, out;
	UBYTE *tmp;
	const char *outname;
	ULONG w, h, fps, total, keyreq, hold;
	ULONG sent, packets, keyframes, bytes;
	ULONG pictures, pts_wrong, held; /* pictures out, those at another time than sent, packets kept */
	ULONG late;                      /* with PACE, pictures sent after their time, a frame not being back */
	BOOL gaps, end_empty, mark_sent, stalled, pace;
	BOOL in_picture; /* the next packet goes on with a picture, whose time came with its first */
	BOOL eos, failed;
	struct VCFrame *waiting; /* with PACE, the frame that waits for its time */
	u32 t_start, t_wait;
};

/**
 * picture_time() - When a picture of the run is.
 * @e: the run
 * @n: the picture
 *
 * Return: its time.
 */
static VCTime picture_time(const struct encode *e, ULONG n)
{
	VCTime t = (VCTime)n * 1000000 / (VCTime)e->fps;
	return e->gaps ? t + (VCTime)(n / GAP_EVERY) * GAP_US : t;
}

/**
 * encode_send() - Send a frame.
 * @e: the run
 * @f: the frame, its Pts and Flags set
 */
static void encode_send(struct encode *e, struct VCFrame *f)
{
	LONG r = VC_SendFrame(e->s, f);
	if (r != 0)
	{
		kv_num("frame.error", r);
		kv_str("fail", "VC_SendFrame failed");
		e->failed = TRUE;
	}
}

/**
 * encode_frame_fill() - A frame takes the next picture of the file and goes.
 * @e: the run
 * @f: the frame
 */
static void encode_frame_fill(struct encode *e, struct VCFrame *f)
{
	if (e->sent >= e->total && e->end_empty && !e->mark_sent)
	{
		e->mark_sent = TRUE;
		f->Pts = picture_time(e, e->sent);
		f->Flags = VCFF_EndOfStream | VCFF_NoPicture;
		encode_send(e, f);
		return;
	}
	if (e->sent >= e->total || !raw_into_frame(e->file, e->tmp, f, e->w, e->h))
		return;
	if (e->keyreq != 0 && e->sent + 1u == e->keyreq)
	{
		/* a key frame on request, before the picture that should start with one */
		LONG r = VC_SetAttrsTags(e->s, VCA_RequestKeyFrame, TRUE, TAG_DONE);
		kv_unum("keyreq.picture", e->keyreq);
		kv_num("keyreq.rc", r);
	}
	f->Pts = picture_time(e, e->sent);
	f->Flags = e->sent + 1u == e->total && !e->end_empty ? VCFF_EndOfStream : 0;
	encode_send(e, f);
	e->sent++;
}

/**
 * picture_due() - How long until the next picture's time, with PACE.
 * @e: the run
 *
 * Return: the microseconds, 0 if it is due or late.
 */
static u32 picture_due(const struct encode *e)
{
	u32 due = (u32)(picture_time(e, e->sent) - picture_time(e, 0));
	u32 gone = get_time() - e->t_start;
	return gone < due ? due - gone : 0;
}

/**
 * encode_frame_back() - A frame the session handed back: filled now, or at its time.
 * @e: the run
 * @f: the frame
 */
static void encode_frame_back(struct encode *e, struct VCFrame *f)
{
	if (e->pace)
		e->waiting = f;
	else
		encode_frame_fill(e, f);
}

/**
 * encode_paced() - Send the frame that waits, when its time has come.
 * @e: the run
 */
static void encode_paced(struct encode *e)
{
	if (e->waiting == NULL || picture_due(e) != 0)
		return;
	u32 gone = get_time() - e->t_start;
	if (gone > (u32)picture_time(e, e->sent) + FRAME_LATE_US)
		e->late++;
	struct VCFrame *f = e->waiting;
	e->waiting = NULL;
	encode_frame_fill(e, f);
}

/**
 * encode_packet() - An encoded packet: written out, counted (packet.N.* for the first three).
 * @e:  the run
 * @pk: the packet
 */
static void encode_packet(struct encode *e, struct VCPacket *pk)
{
	if (pk->Length != 0 && Write(e->out, pk->Data, (LONG)pk->Length) != (LONG)pk->Length)
	{
		kv_str("fail", "writing the stream failed");
		e->failed = TRUE;
	}
	e->bytes += pk->Length;
	if (e->packets < 3)
	{
		kv_unum(kv_at("packet", e->packets, "bytes"), pk->Length);
		kv_hex(kv_at("packet", e->packets, "flags"), pk->Flags);
		kv_unum(kv_at("packet", e->packets, "pts"), (ULONG)pk->Pts);
	}
	e->packets++;
	if (pk->Flags & VCPF_KeyFrame)
		e->keyframes++;
	if (pk->Length != 0)
	{
		VCTime want = picture_time(e, e->pictures);
		if (!e->in_picture && pk->Pts != want && e->pts_wrong++ == 0)
		{
			kv_unum("encode.pts_first_wrong", e->pictures);
			kv_unum("encode.pts_got", (ULONG)pk->Pts);
			kv_unum("encode.pts_want", (ULONG)want);
		}
		e->in_picture = (pk->Flags & VCPF_FrameEnd) == 0;
		if (!e->in_picture)
			e->pictures++;
	}
	if (e->hold != 0 && e->packets > e->hold)
	{
		e->held++;
		return;
	}
	VC_ReleasePacket(e->s, pk);
}

/**
 * encode_loop() - Collect, feed, wait, until the end of the stream or a failure.
 * @e: the run
 */
static void encode_loop(struct encode *e)
{
	for (;;)
	{
		struct VCItem it;
		while (!e->failed && !e->eos && VC_Poll(e->s, &it))
		{
			switch (it.Type)
			{
			case VCIT_FrameReturned:
				encode_frame_back(e, it.u.Frame);
				break;
			case VCIT_Packet:
				encode_packet(e, it.u.Packet);
				break;
			case VCIT_Event:
				if (it.u.Event.Event == VCEV_EndOfStream)
					e->eos = TRUE;
				else if (it.u.Event.Event == VCEV_Error)
				{
					kv_num("session.error", it.u.Event.Status);
					kv_str("fail", "the session reported an error");
					e->failed = TRUE;
				}
				break;
			default:
				break;
			}
		}
		if (e->failed || e->eos)
			return;
		encode_paced(e);
		u32 w0 = get_time();
		ULONG idle_ms = e->waiting != NULL ? picture_due(e) / 1000u : e->hold != 0 ? HOLD_MS : IDLE_MS;
		ULONG seen = waiter_wait(&e->t->waiter, 0, idle_ms);
		e->t_wait += get_time() - w0;
		if (seen == 0 && e->held != 0)
		{
			e->stalled = TRUE;
			return;
		}
		if (seen == 0 && e->waiting != NULL)
			continue;
		if ((seen & waiter_sigmask(&e->t->waiter)) == 0)
		{
			kv_bool("encode.idle", seen == 0);
			kv_str("fail", seen == 0 ? "nothing happened" : "stopped");
			note("nothing happened for %lu ms (or Ctrl-C): %lu pictures sent, %lu packets back\n", (ULONG)IDLE_MS, e->sent, e->packets);
			e->failed = TRUE;
			return;
		}
	}
}

/**
 * do_encode() - ENCODE: the raw pictures through an H.264 encoder session.
 * @t:    the tool
 * @args: encode_template's
 *
 * Return: RETURN_OK; RETURN_FAIL for a refused session or a failure; RETURN_ERROR for a missing file or memory.
 */
int do_encode(struct tool *t, LONG *args)
{
	struct encode e = {0};
	const char *raw = arg_str(args, ARG_FILE);
	e.t = t;
	e.outname = arg_str(args, ARG_OUT);
	e.w = arg_num(args, ARG_WIDTH, 0);
	e.h = arg_num(args, ARG_HEIGHT, 0);
	e.fps = arg_num(args, ARG_FPS, 30u);
	e.keyreq = arg_num(args, ARG_KEYREQ, 0);
	e.hold = arg_num(args, ARG_HOLD, 0);
	e.gaps = args[ARG_GAPS] != 0;
	e.end_empty = args[ARG_ENDEMPTY] != 0;
	e.pace = args[ARG_PACE] != 0;
	ULONG bitrate = arg_num(args, ARG_BITRATE, 0), keyint = arg_num(args, ARG_KEYINT, 0);
	ULONG out_w = arg_num(args, ARG_OUTWIDTH, 0), out_h = arg_num(args, ARG_OUTHEIGHT, 0), packet = arg_num(args, ARG_PACKET, 0);
	const char *format_arg = arg_str(args, ARG_FORMAT);
	ULONG format = format_arg != NULL ? format_of_name(format_arg) : VCF_I420;
	t->backend = arg_str(args, ARG_BACKEND);
	if (e.w == 0 || e.h == 0)
	{
		kv_str("fail", "ENCODE needs WIDTH and HEIGHT");
		return RETURN_FAIL;
	}
	if (format == 0)
	{
		kv_str("fail", "no such format");
		return RETURN_FAIL;
	}
	ULONG picture_bytes = raw_bytes(format, e.w, e.h);
	e.file = Open((CONST_STRPTR)raw, MODE_OLDFILE);
	if (e.file == 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR)raw);
		kv_str("fail", "the raw file did not open");
		return RETURN_ERROR;
	}
	Seek(e.file, 0, OFFSET_END);
	LONG fsize = Seek(e.file, 0, OFFSET_BEGINNING);
	e.total = fsize > 0 ? (ULONG)fsize / picture_bytes : 0;
	kv_str("encode.file", raw);
	kv_unum("encode.width", e.w);
	kv_unum("encode.height", e.h);
	kv_unum("encode.fps_set", e.fps);
	kv_unum("encode.bitrate", bitrate);
	kv_unum("encode.keyint", keyint);
	kv_unum("encode.total", e.total);
	int rc = RETURN_ERROR;
	e.out = Open((CONST_STRPTR)e.outname, MODE_NEWFILE);
	e.tmp = AllocVec(picture_bytes, MEMF_PUBLIC);
	if (e.out == 0 || e.tmp == NULL)
	{
		if (e.out == 0)
			PrintFault(IoErr(), (CONST_STRPTR)e.outname);
		kv_str("fail", e.out == 0 ? "the output file did not open" : "no memory");
		goto out_files;
	}
	LONG err = 0;
	e.s = VC_OpenSessionTags(VCA_Codec, VCC_H264, VCA_Kind, VCK_Encoder, VCA_Width, e.w, VCA_Height, e.h, VCA_InputFormat, format,
	                         VCA_OutputWidth, out_w, VCA_OutputHeight, out_h, VCA_FrameRateNum, e.fps, VCA_FrameRateDen, 1, VCA_Bitrate, bitrate, VCA_KeyInterval, keyint, VCA_SigTask,
	                         (ULONG)t->waiter.task, VCA_SigMask, waiter_sigmask(&t->waiter), VCA_InputCount, 2, VCA_OutputCount, 4, VCA_OutputSize, packet, BACKEND_TAG(t),
	                         VCA_ErrorPtr, (ULONG)&err, TAG_DONE);
	kv_str("encode.session", e.s != NULL ? "open" : "refused");
	if (e.s == NULL)
	{
		kv_num("error", err);
		kv_str("fail", "the session was refused");
		rc = RETURN_FAIL;
		goto out_files;
	}

	e.t_start = get_time();
	encode_loop(&e);
	u32 total_us = get_time() - e.t_start;
	ULONG ms = total_us / 1000u;
	kv_unum("encode.pictures", e.sent);
	kv_unum("encode.pictures_out", e.pictures);
	kv_unum("encode.pts_wrong", e.pts_wrong);
	kv_unum("encode.held", e.held);
	kv_unum("encode.late", e.late);
	kv_bool("encode.end", e.eos);
	kv_unum("encode.packets", e.packets);
	kv_unum("encode.keyframes", e.keyframes);
	kv_unum("encode.bytes", e.bytes);
	kv_unum("encode.ms", ms);
	kv_fixed1("encode.fps", e.sent * 1000u, ms);
	kv_unum("encode.kbit", e.sent != 0 ? e.bytes * 8u / 1000u * e.fps / e.sent : 0);
	kv_unum("encode.busy_pct", total_us != 0 ? (ULONG)(((u64)(total_us - e.t_wait)) * 100u / total_us) : 0);
	if (e.failed)
		rc = RETURN_FAIL;
	else if (e.stalled)
		rc = RETURN_OK;
	else if (!e.eos || e.sent != e.total || e.total == 0)
	{
		kv_str("fail", e.total == 0 ? "no pictures in the file" : "the stream ended early");
		rc = RETURN_FAIL;
	}
	else
		rc = RETURN_OK;
	u32 t_close = get_time();
	VC_CloseSession(e.s);
	kv_unum("encode.close_ms", (get_time() - t_close) / 1000u);
out_files:
	if (e.tmp != NULL)
		FreeVec(e.tmp);
	if (e.out != 0)
		Close(e.out);
	Close(e.file);
	return rc;
}
