/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * decode.c - DECODE: an elementary stream through a decoder session.  Every
 * I420 frame is hashed like ffmpeg's framemd5 and compared with REF; an
 * OUTFORMAT other than I420 takes the frames through the ISP, counted and
 * not hashed, except NV12_COL128, an H.265 decoder's native column frames,
 * hashed whole and compared with a REF made by the backend's colmd5.py.
 * FRAMED sends one access unit per packet with timestamps,
 * FLUSH n calls VC_Flush() after n frames and sends the stream again from
 * its start (timestamps 1000 s on: a frame with an older one after the flush
 * is stale).  EXTRADATA (with FRAMED) takes the parameter sets before the
 * first picture (H.264's SPS and PPS, H.265's VPS, SPS and PPS) out of the
 * stream and passes them as VCA_Extradata, PAD n zero bytes after them
 * (Annex-B's trailing_zero_8bits) to make it larger; CONFIG file passes a
 * file as VCA_Extradata instead (an avcC or hvcC record), TAG fourcc opens
 * the session with it as VCA_CodecTag.  NOPTS
 * (with FRAMED) sends the access units without timestamps, NORATE opens the
 * session without a frame rate, PACKET n sets the size of an input packet
 * (the backend imports them from 512 KiB on).  HASHES prints every frame's
 * hash (frame.N.md5), for a host that matches frames by content when a
 * damaged stream loses some.  OUT writes the frames: I420's planes as
 * ffmpeg's rawvideo packs them, a column frame whole, a packed format's
 * visible rows.  A .jpg goes as one MJPEG picture.
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

#include <args.h>
#include <kv.h>
#include <md5.h>
#include <timing.h>
#include <vc_names.h>

#include "commands.h"

const char decode_template[] =
    "FILE/A,REF/K,OUT/K,FRAMES/N,NOHASH/S,ENCODING/K,VERBOSE/S,FRAMED/S,FPS/N,OUTFORMAT/K,FLUSH/N,BACKEND/K,EXTRADATA/S,PAD/N,NOPTS/S,NORATE/S,PACKET/N,"
    "HASHES/S,CONFIG/K,TAG/K";

enum
{
	ARG_FILE,
	ARG_REF,
	ARG_OUT,
	ARG_FRAMES,
	ARG_NOHASH,
	ARG_ENCODING,
	ARG_VERBOSE,
	ARG_FRAMED,
	ARG_FPS,
	ARG_OUTFORMAT,
	ARG_FLUSH,
	ARG_BACKEND,
	ARG_EXTRADATA,
	ARG_PAD,
	ARG_NOPTS,
	ARG_NORATE,
	ARG_PACKET,
	ARG_HASHES,
	ARG_CONFIG,
	ARG_TAG
};

#define DECODE_PACKETS 4               /* the session's input packets */
#define FLUSH_PTS ((VCTime)1000000000) /* FLUSH: the restarted stream's timestamps, 1000 s on */
#define NO_LIMIT 0xffffffffu

struct decode
{
	struct tool *t;
	struct VCSession *s;
	BPTR file;
	BPTR outfile;
	ULONG max_frames;
	BOOL hash, verbose, framed, nopts, norate;
	BOOL hevc;   /* H.265: its NAL headers and access unit rules */
	BOOL hashes; /* HASHES: every frame's hash printed */
	ULONG fps;
	UBYTE (*ref)[16];
	ULONG ref_count;
	/* FRAMED: the whole stream in memory, cut into access units */
	UBYTE *stream;
	ULONG stream_len, scan_pos, au_pos, au_len, sent_frames;
	/* EXTRADATA: the SPS and PPS taken out of the stream, and PAD's zeros */
	UBYTE *config;
	ULONG config_len;
	struct md5 in_md5;
	struct VCStreamInfo si;
	ULONG streams; /* StreamInfo events: stream.N.* */
	ULONG frames, mismatches, in_bytes, in_buffers;
	ULONG first_after; /* packets sent when the first frame came: the decoder's start latency */
	u64 out_bytes; /* past 4 GB in a long run */
	ULONG file_size; /* the stream's bytes: the packet that reaches it carries the end mark */
	BOOL eof_sent, eos_seen, failed, geometry_printed, have_stream;
	u32 t_hash, t_wait;
	UBYTE digest[16];
	ULONG outfmt; /* the frames' format: I420 hashed, any other counted (through the ISP) */
	/* FLUSH: VC_Flush() after flush_at frames, then the stream again from its start, its timestamps FLUSH_PTS on */
	ULONG flush_at, au_index, after_flush, stale, untimed;
	LONG flush_rc;
	BOOL flushed;
	struct VCPacket *idle[DECODE_PACKETS]; /* packets the session handed back with nothing left to send */
	ULONG idle_n;
};

/**
 * next_nal() - The Annex-B NAL unit at or after *@pos, start code included.
 * @s:     the stream
 * @len:   its length
 * @pos:   where to look from; moved past the unit
 * @start: where the unit's start goes (a 4-byte start code's leading zero goes with its own NAL)
 * @end:   where its end goes
 *
 * Return: FALSE at the end.
 */
static BOOL next_nal(const UBYTE *s, ULONG len, ULONG *pos, ULONG *start, ULONG *end)
{
	ULONG i = *pos;
	while (i + 3 <= len && !(s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1))
		i++;
	if (i + 3 > len)
		return FALSE;
	ULONG j = i + 3;
	while (j + 3 <= len && !(s[j] == 0 && s[j + 1] == 0 && s[j + 2] == 1))
		j++;
	ULONG e = j + 3 <= len ? j : len;
	if (j + 3 <= len && e > i + 3 && s[e - 1] == 0)
		e--;
	*start = (i > 0 && s[i - 1] == 0) ? i - 1 : i;
	*end = e;
	*pos = e;
	return TRUE;
}

/**
 * nal_kind() - What a NAL unit is to the access unit splitter.
 * @d:     the decode, its stream in memory
 * @hdr:   where the unit's header starts, past its start code
 * @vcl:   set for a slice
 * @opens: set when the unit, after a picture's slices, opens the next access unit
 *
 * H.264 (7.4.1.2.3): a slice with first_mb_in_slice 0 (its first header bit
 * set, 7.3.3), or any unit but a slice.  H.265 (7.4.2.4.4): a slice with
 * first_slice_segment_in_pic_flag, a VPS, SPS, PPS, access unit delimiter,
 * prefix SEI, or a reserved (41 to 44) or unspecified (48 to 55) unit; the
 * end of a sequence or of the bitstream, filler data and a suffix SEI stay
 * with the access unit they follow.
 */
static void nal_kind(const struct decode *d, ULONG hdr, BOOL *vcl, BOOL *opens)
{
	const UBYTE *s = d->stream;
	ULONG left = hdr < d->stream_len ? d->stream_len - hdr : 0;
	if (d->hevc)
	{
		UBYTE type = left > 0 ? (UBYTE)((s[hdr] >> 1) & 0x3f) : 0;
		*vcl = left > 0 && type < 32;
		if (*vcl)
			*opens = left > 2 && (s[hdr + 2] & 0x80) != 0;
		else
			*opens = (type >= 32 && type <= 35) || type == 39 || (type >= 41 && type <= 44) || (type >= 48 && type <= 55);
		return;
	}
	UBYTE type = left > 0 ? (UBYTE)(s[hdr] & 0x1f) : 0;
	*vcl = type >= 1 && type <= 5;
	*opens = !*vcl || (left > 1 && (s[hdr + 1] & 0x80) != 0);
}

/**
 * next_au() - The next access unit of the stream in memory.
 * @d:        the decode
 * @au_start: where it starts
 * @au_end:   where it ends
 *
 * NAL units up to the one that opens the next picture (nal_kind()).
 *
 * Return: FALSE at the end.
 */
static BOOL next_au(struct decode *d, ULONG *au_start, ULONG *au_end)
{
	const UBYTE *s = d->stream;
	ULONG start = d->scan_pos, end = start;
	BOOL have_vcl = FALSE;

	if (start >= d->stream_len)
		return FALSE;
	for (;;)
	{
		ULONG p = d->scan_pos, ns, ne;
		if (!next_nal(s, d->stream_len, &p, &ns, &ne))
		{
			end = d->stream_len;
			d->scan_pos = d->stream_len;
			break;
		}
		ULONG hdr = ns + (s[ns + 2] == 1 ? 3u : 4u);
		BOOL vcl, opens;
		nal_kind(d, hdr, &vcl, &opens);
		if (have_vcl && opens)
			break; /* this NAL belongs to the next picture: it is found again */
		if (vcl)
			have_vcl = TRUE;
		end = ne;
		d->scan_pos = p;
	}
	*au_start = start;
	*au_end = end;
	return end > start;
}

/**
 * split_config() - EXTRADATA: the parameter sets before the first picture, out of the stream and into the configuration.
 * @d:   the decode, its stream in memory
 * @pad: zero bytes to put after them
 *
 * The stream closes up behind the NAL units taken out.
 *
 * Return: FALSE without memory, or with no SPS or PPS before the first slice.
 */
static BOOL split_config(struct decode *d, ULONG pad)
{
	UBYTE *s = d->stream;
	d->config = AllocVec(d->stream_len + pad, MEMF_ANY | MEMF_CLEAR);
	if (d->config == NULL)
		return FALSE;
	ULONG pos = 0, ns, ne, kept = 0, done = 0, taken = 0;
	while (next_nal(s, d->stream_len, &pos, &ns, &ne))
	{
		ULONG hdr = ns + (s[ns + 2] == 1 ? 3u : 4u);
		BOOL vcl, set;
		if (d->hevc)
		{
			UBYTE type = hdr < d->stream_len ? (UBYTE)((s[hdr] >> 1) & 0x3f) : 32;
			vcl = type < 32;
			set = type >= 32 && type <= 34; /* VPS, SPS, PPS */
		}
		else
		{
			UBYTE type = hdr < d->stream_len ? (UBYTE)(s[hdr] & 0x1f) : 0;
			vcl = type >= 1 && type <= 5;
			set = type == 7 || type == 8; /* SPS, PPS */
		}
		if (vcl)
			break; /* the first slice */
		if (!set)
			continue;
		memcpy(d->config + taken, s + ns, ne - ns);
		taken += ne - ns;
		memmove(s + kept, s + done, ns - done); /* what came before it stays */
		kept += ns - done;
		done = ne;
	}
	memmove(s + kept, s + done, d->stream_len - done);
	d->stream_len = kept + d->stream_len - done;
	d->config_len = taken + pad; /* the zeros are there already */
	return taken != 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/**
 * load_ref() - Read ffmpeg's framemd5 output: one line per frame, the digest last.
 * @d:    the decode
 * @name: the file; comment lines start with '#'
 *
 * Return: TRUE with d->ref filled.
 */
static BOOL load_ref(struct decode *d, const char *name)
{
	BPTR f = Open((CONST_STRPTR)name, MODE_OLDFILE);
	if (f == 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR)name);
		return FALSE;
	}
	Seek(f, 0, OFFSET_END);
	LONG size = Seek(f, 0, OFFSET_BEGINNING);
	char *text = size > 0 ? AllocVec((ULONG)size + 1u, MEMF_ANY) : NULL;
	if (text == NULL || Read(f, text, size) != size)
	{
		note("cannot read %s\n", name);
		Close(f);
		if (text != NULL)
			FreeVec(text);
		return FALSE;
	}
	Close(f);
	text[size] = 0;

	ULONG lines = 0;
	for (LONG i = 0; i < size; i++)
		if (text[i] == '\n')
			lines++;
	d->ref = AllocVec((lines + 1u) * 16u, MEMF_ANY);
	if (d->ref == NULL)
	{
		FreeVec(text);
		return FALSE;
	}

	LONG pos = 0;
	while (pos < size)
	{
		LONG end = pos;
		while (end < size && text[end] != '\n')
			end++;
		LONG last = end;
		while (last > pos && (text[last - 1] == '\r' || text[last - 1] == ' '))
			last--;
		if (text[pos] != '#' && last - pos >= 32)
		{
			UBYTE *dig = d->ref[d->ref_count];
			BOOL ok = TRUE;
			for (LONG k = 0; k < 16 && ok; k++)
			{
				int hi = hexval(text[last - 32 + 2 * k]), lo = hexval(text[last - 32 + 2 * k + 1]);
				ok = hi >= 0 && lo >= 0;
				dig[k] = (UBYTE)((hi << 4) | lo);
			}
			if (ok)
				d->ref_count++;
		}
		pos = end + 1;
	}
	FreeVec(text);
	kv_unum("ref.count", d->ref_count);
	return d->ref_count > 0;
}

static void digest_text(const UBYTE *dig, char out[33])
{
	static const char hex[] = "0123456789abcdef";
	for (ULONG i = 0; i < 16; i++)
	{
		out[2 * i] = hex[dig[i] >> 4];
		out[2 * i + 1] = hex[dig[i] & 15];
	}
	out[32] = 0;
}

/**
 * plane_rows() - One plane's visible rows: hashed, and written out, as ffmpeg's rawvideo packs them.
 * @d:     the decode
 * @m:     the frame's digest
 * @base:  the plane
 * @pitch: its bytes per row
 * @x:     the visible area's left, in bytes
 * @y:     its top row
 * @w:     its bytes per row
 * @rows:  its rows
 */
static void plane_rows(struct decode *d, struct md5 *m, const UBYTE *base, ULONG pitch, ULONG x, ULONG y, ULONG w, ULONG rows)
{
	if (x == 0 && pitch == w)
	{
		if (d->hash)
			md5_update(m, base + y * pitch, w * rows);
		if (d->outfile != 0)
			Write(d->outfile, (APTR)(base + y * pitch), (LONG)(w * rows));
		return;
	}
	for (ULONG r = 0; r < rows; r++)
	{
		const UBYTE *p = base + (y + r) * pitch + x;
		if (d->hash)
			md5_update(m, p, w);
		if (d->outfile != 0)
			Write(d->outfile, (APTR)p, (LONG)w);
	}
}

/**
 * packed_bytes() - Bytes a pixel of a packed format.
 * @fmt: VCF_*, not a planar one
 *
 * Return: 2, 3 or 4.
 */
static ULONG packed_bytes(ULONG fmt)
{
	switch (fmt)
	{
	case VCF_RGB24:
	case VCF_BGR24:
		return 3;
	case VCF_BGRA32:
	case VCF_RGBA32:
		return 4;
	default:
		return 2; /* RGB16, YUYV, UYVY */
	}
}

/**
 * handle_frame() - A decoded frame: its visible area hashed and written, planes Y U V back to back as ffmpeg does.
 * @d: the decode
 * @f: the frame
 */
static void handle_frame(struct decode *d, const struct VCFrame *f)
{
	LONG cx = f->Crop.X, cy = f->Crop.Y, cw = f->Crop.Width, ch = f->Crop.Height;
	if (cw <= 0 || ch <= 0 || cx < 0 || cy < 0 || (d->outfmt == VCF_I420 && f->Planes < 3))
	{
		kv_str("fail", "a frame without a picture");
		note("frame %lu: no picture in it (%ldx%ld, %lu planes)\n", d->frames, cw, ch, f->Planes);
		d->failed = TRUE;
		return;
	}
	if (d->frames == 0)
		d->first_after = d->in_buffers;
	if (!d->geometry_printed)
	{
		kv_num("frame.first.width", cw);
		kv_num("frame.first.height", ch);
		kv_num("frame.first.x", cx);
		kv_num("frame.first.y", cy);
		kv_unum("frame.first.mem_width", f->Width);
		kv_unum("frame.first.mem_height", f->Height);
		kv_unum("frame.first.planes", f->Planes);
		kv_unum("frame.first.pitch", f->Pitch[0]);
		kv_hex("frame.first.flags", f->Flags);
		note("first frame: pitch %lu/%lu, offsets %lu/%lu/%lu, pts %lu\n", f->Pitch[0], f->Pitch[1], f->Offset[0], f->Offset[1],
		     f->Offset[2], (ULONG)f->Pts);
		d->geometry_printed = TRUE;
	}
	if (d->flushed && f->Pts == VC_TIME_UNKNOWN)
		d->untimed++;
	else if (d->flushed && f->Pts < FLUSH_PTS)
	{
		if (d->stale++ < 3)
			note("frame %lu after the flush is stale: pts %lu\n", d->frames, (ULONG)f->Pts);
	}
	else if (d->flushed)
		d->after_flush++;
	if (d->outfmt != VCF_I420 && d->outfmt != VCF_NV12_COL128)
	{
		d->out_bytes += (u64)f->Pitch[0] * (ULONG)ch; /* a packed picture: counted, not hashed, written as its visible rows */
		if (d->outfile != 0)
		{
			ULONG bpp = packed_bytes(d->outfmt);
			const UBYTE *rows = (const UBYTE *)f->Data + f->Offset[0] + (ULONG)cx * bpp;
			for (LONG r = cy; r < cy + ch; r++)
				Write(d->outfile, (APTR)(rows + (ULONG)r * f->Pitch[0]), (LONG)((ULONG)cw * bpp));
		}
		d->frames++;
		return;
	}

	const UBYTE *data = f->Data;
	struct md5 m;
	md5_init(&m);
	u32 t0 = get_time();
	if (d->outfmt == VCF_NV12_COL128)
	{
		/* the column frame: the visible window hashed column by column, luma lines then chroma lines, as the
		   backend's colmd5.py hashes ffmpeg's output (the padding the decoder writes is left out); written out whole */
		ULONG columns = f->Width / 128u, rows = f->Offset[1] / 128u, bytes = f->Pitch[0] * columns;
		for (ULONG c = 0; c < columns && d->hash; c++)
		{
			LONG b0 = cx - (LONG)(c * 128u), b1 = cx + cw - (LONG)(c * 128u);
			const UBYTE *col = data + c * f->Pitch[0];
			if (b0 < 0)
				b0 = 0;
			if (b1 > 128)
				b1 = 128;
			if (b1 <= b0)
				continue;
			for (LONG r = cy; r < cy + ch; r++)
				md5_update(&m, col + r * 128 + b0, (ULONG)(b1 - b0));
			for (LONG r = cy / 2; r < (cy + ch) / 2; r++)
				md5_update(&m, col + ((LONG)rows + r) * 128 + b0, (ULONG)(b1 - b0));
		}
		if (d->outfile != 0)
			Write(d->outfile, (APTR)data, (LONG)bytes);
		d->out_bytes += bytes;
	}
	else
	{
		plane_rows(d, &m, data + f->Offset[0], f->Pitch[0], (ULONG)cx, (ULONG)cy, (ULONG)cw, (ULONG)ch);
		plane_rows(d, &m, data + f->Offset[1], f->Pitch[1], (ULONG)cx / 2u, (ULONG)cy / 2u, (ULONG)cw / 2u, (ULONG)ch / 2u);
		plane_rows(d, &m, data + f->Offset[2], f->Pitch[2], (ULONG)cx / 2u, (ULONG)cy / 2u, (ULONG)cw / 2u, (ULONG)ch / 2u);
		d->out_bytes += (u64)cw * (ULONG)ch * 3u / 2u;
	}
	if (d->hash)
		md5_final(&m, d->digest);
	d->t_hash += get_time() - t0;
	if (d->hash && d->hashes)
	{
		char a[33];
		digest_text(d->digest, a);
		kv_str(kv_at("frame", d->frames, "md5"), a);
	}

	if (d->hash && d->ref != NULL && d->frames < d->ref_count)
	{
		BOOL same = TRUE;
		for (ULONG i = 0; i < 16; i++)
			same = same && d->digest[i] == d->ref[d->frames][i];
		if (!same)
		{
			if (d->mismatches < 3 || d->verbose) /* the first three, or every one */
			{
				char a[33], e[33];
				digest_text(d->digest, a);
				digest_text(d->ref[d->frames], e);
				kv_unum(kv_at("mismatch", d->mismatches, "frame"), d->frames);
				kv_str(kv_at("mismatch", d->mismatches, "md5"), a);
				kv_str(kv_at("mismatch", d->mismatches, "ref"), e);
			}
			d->mismatches++;
		}
	}
	else if (d->hash && d->ref == NULL && (d->frames < 2 || (d->frames % 100u) == 0))
	{
		char a[33];
		digest_text(d->digest, a);
		note("frame %lu: md5 %s\n", d->frames, a);
	}
	d->frames++;
}

/**
 * fill_packet() - A packet the session handed back: the next piece of the stream, or the end mark once.
 * @d:  the decode
 * @pk: the packet
 */
static void fill_packet(struct decode *d, struct VCPacket *pk)
{
	if (d->eof_sent || d->failed)
	{
		if (d->idle_n < DECODE_PACKETS)
			d->idle[d->idle_n++] = pk; /* nothing more to send: idle, unless a FLUSH sends the stream again */
		return;
	}
	ULONG flags = 0;
	VCTime ts = VC_TIME_UNKNOWN;
	LONG n;
	if (d->framed)
	{
		/* one access unit per packet (split over several when larger), with timestamps in decode order */
		if (d->au_len == 0)
		{
			ULONG s, e;
			if (next_au(d, &s, &e))
			{
				d->au_pos = s;
				d->au_len = e - s;
				flags |= VCPF_FrameStart;
			}
		}
		n = (LONG)(d->au_len < pk->Capacity ? d->au_len : pk->Capacity);
		if (n > 0)
		{
			for (LONG i = 0; i < n; i++)
				((UBYTE *)pk->Data)[i] = d->stream[d->au_pos + (ULONG)i];
			d->au_pos += (ULONG)n;
			d->au_len -= (ULONG)n;
			if (!d->nopts)
				ts = (d->flushed ? FLUSH_PTS : 0) + (VCTime)d->au_index * 1000000 / (VCTime)d->fps;
			if (d->au_len == 0)
			{
				flags |= VCPF_FrameEnd;
				d->sent_frames++;
				d->au_index++;
			}
		}
	}
	else
	{
		n = Read(d->file, pk->Data, (LONG)pk->Capacity);
		if (n < 0)
		{
			PrintFault(IoErr(), (CONST_STRPTR) "read");
			kv_str("fail", "reading the stream failed");
			d->failed = TRUE;
			return;
		}
	}
	if (n > 0)
		md5_update(&d->in_md5, pk->Data, (ULONG)n);
	/* the end mark goes with the file's last data: the JPEG decoder ignores an empty one */
	BOOL last = n == 0 || (!d->framed && d->in_bytes + (ULONG)n >= d->file_size);
	pk->Length = (ULONG)n;
	pk->Flags = last ? flags | VCPF_EndOfStream : flags;
	pk->Pts = ts;
	if (last)
		d->eof_sent = TRUE;
	LONG rc = VC_SendPacket(d->s, pk);
	if (rc != 0)
	{
		kv_num("packet.error", rc);
		kv_str("fail", "VC_SendPacket failed");
		d->failed = TRUE;
		return;
	}
	d->in_bytes += (ULONG)n;
	d->in_buffers++;
}

/**
 * decode_flush() - FLUSH: drop what the session holds, then send the stream again from its start.
 * @d: the decode
 */
static void decode_flush(struct decode *d)
{
	d->flushed = TRUE;
	d->scan_pos = 0;
	d->au_len = 0;
	d->au_index = 0;
	d->eof_sent = FALSE;
	d->flush_rc = VC_Flush(d->s);
	kv_unum("flush.after", d->frames);
	kv_num("flush.rc", d->flush_rc);
	if (d->flush_rc != 0)
	{
		kv_str("fail", "VC_Flush failed");
		d->failed = TRUE;
		return;
	}
	ULONG idle_n = d->idle_n; /* fill_packet() may park them again */
	d->idle_n = 0;
	for (ULONG i = 0; i < idle_n; i++)
		fill_packet(d, d->idle[i]);
}

/**
 * handle_event() - A session event: the stream's shape (stream.N.*), its end, or an error.
 * @d: the decode
 * @e: the event
 */
static void handle_event(struct decode *d, const struct VCEvent *e)
{
	switch (e->Event)
	{
	case VCEV_StreamInfo:
		d->si = e->Stream;
		d->have_stream = TRUE;
		d->geometry_printed = FALSE;
		kv_str(kv_at("stream", d->streams, "format"), format_name(e->Stream.Format));
		kv_num(kv_at("stream", d->streams, "width"), e->Stream.Crop.Width);
		kv_num(kv_at("stream", d->streams, "height"), e->Stream.Crop.Height);
		kv_unum(kv_at("stream", d->streams, "mem_width"), e->Stream.Width);
		kv_unum(kv_at("stream", d->streams, "mem_height"), e->Stream.Height);
		kv_num(kv_at("stream", d->streams, "fps_num"), e->Stream.FrameRate.Num);
		kv_num(kv_at("stream", d->streams, "fps_den"), e->Stream.FrameRate.Den);
		kv_num(kv_at("stream", d->streams, "par_num"), e->Stream.Par.Num);
		kv_num(kv_at("stream", d->streams, "par_den"), e->Stream.Par.Den);
		kv_hex(kv_at("stream", d->streams, "colorspace"), e->Stream.ColorSpace);
		d->streams++;
		break;
	case VCEV_EndOfStream:
		d->eos_seen = TRUE;
		break;
	case VCEV_Error:
		kv_num("session.error", e->Status);
		kv_str("fail", "the session reported an error");
		d->failed = TRUE;
		break;
	default:
		note("event %lu\n", e->Event);
		break;
	}
}

/**
 * codec_of() - The codec ENCODING names, or the file's extension tells.
 * @name: ENCODING, or NULL
 * @file: the stream's name
 *
 * Return: VCC_H264, VCC_H265 (H265 or HEVC; .h265, .hevc, .265) or VCC_MJPEG (a .jpg too: one picture per packet
 * through the video decoder); VCC_None for a bad name.
 */
static ULONG codec_of(const char *name, const char *file)
{
	if (name != NULL)
	{
		if ((name[0] == 'H' || name[0] == 'h') && name[1] == '2' && name[2] == '6' && name[3] == '5')
			return VCC_H265;
		if ((name[0] == 'H' || name[0] == 'h') && (name[1] == 'E' || name[1] == 'e'))
			return VCC_H265;
		if (name[0] == 'H' || name[0] == 'h')
			return VCC_H264;
		if ((name[0] == 'M' || name[0] == 'm') && (name[1] == 'J' || name[1] == 'j'))
			return VCC_MJPEG;
		return VCC_None;
	}
	u32 n = (u32)strlen(file);
	const char *ext = file + n;
	while (ext > file && ext[-1] != '.' && ext[-1] != '/' && ext[-1] != ':')
		ext--;
	BOOL jpeg = (ext[0] == 'j' || ext[0] == 'J') && (ext[1] == 'p' || ext[1] == 'P');
	BOOL mjpeg = (ext[0] == 'm' || ext[0] == 'M') && (ext[1] == 'j' || ext[1] == 'J');
	BOOL hevc = ((ext[0] == 'h' || ext[0] == 'H') && ext[1] == '2' && ext[2] == '6' && ext[3] == '5') ||
	            ((ext[0] == 'h' || ext[0] == 'H') && (ext[1] == 'e' || ext[1] == 'E')) || (ext[0] == '2' && ext[1] == '6' && ext[2] == '5');
	return jpeg || mjpeg ? VCC_MJPEG : hevc ? VCC_H265 : VCC_H264;
}

/**
 * decode_loop() - Collect, feed, wait, until the end, a failure or FRAMES.
 * @d: the decode
 */
static void decode_loop(struct decode *d)
{
	for (;;)
	{
		struct VCItem it;
		while (!d->failed && !d->eos_seen && d->frames < d->max_frames && VC_Poll(d->s, &it))
		{
			switch (it.Type)
			{
			case VCIT_PacketReturned:
				fill_packet(d, it.u.Packet);
				break;
			case VCIT_Frame:
				if (d->verbose)
					note("frame back: %ldx%ld, pts %lu, flags %lx\n", it.u.Frame->Crop.Width, it.u.Frame->Crop.Height, (ULONG)it.u.Frame->Pts,
					     it.u.Frame->Flags);
				handle_frame(d, it.u.Frame);
				VC_ReleaseFrame(d->s, it.u.Frame);
				if (d->flush_at != 0 && !d->flushed && d->frames >= d->flush_at)
					decode_flush(d);
				break;
			case VCIT_Event:
				handle_event(d, &it.u.Event);
				break;
			default:
				note("item %lu?\n", it.Type);
				break;
			}
		}
		if (d->failed || d->eos_seen || d->frames >= d->max_frames)
			return;
		u32 w0 = get_time();
		ULONG seen = waiter_wait(&d->t->waiter, 0, IDLE_MS);
		d->t_wait += get_time() - w0;
		if ((seen & waiter_sigmask(&d->t->waiter)) == 0)
		{
			kv_bool("decode.idle", seen == 0);
			kv_str("fail", seen == 0 ? "nothing happened" : "stopped");
			note("nothing happened for %lu ms (or Ctrl-C): %lu frames, input %lu bytes in %lu packets%s\n", (ULONG)IDLE_MS, d->frames,
			     d->in_bytes, d->in_buffers, d->have_stream ? "" : " (no stream shape yet: not a stream the decoder takes)");
			d->failed = TRUE;
			return;
		}
	}
}

/**
 * decode_summary() - The run's numbers, and its verdict.
 * @d:     the decode
 * @total: the run's microseconds
 *
 * Return: RETURN_OK; RETURN_FAIL when it failed; RETURN_WARN when a check failed (fail=).
 */
static int decode_summary(struct decode *d, u32 total)
{
	ULONG ms = total / 1000u;
	kv_unum("decode.frames", d->frames);
	kv_num("decode.width", d->si.Crop.Width);
	kv_num("decode.height", d->si.Crop.Height);
	kv_unum("decode.ms", ms);
	kv_fixed1("decode.fps", d->frames * 1000u, ms);
	kv_unum("decode.in_bytes", d->in_bytes);
	kv_unum("decode.in_packets", d->in_buffers);
	kv_unum("decode.first_frame_after", d->first_after);
	kv_unum("decode.out_kib", (ULONG)(d->out_bytes >> 10));
	kv_unum("decode.hash_ms", d->t_hash / 1000u);
	kv_unum("decode.wait_ms", d->t_wait / 1000u);
	kv_unum("decode.busy_pct", total != 0 ? (ULONG)(((u64)(total - d->t_wait)) * 100u / total) : 0);
	kv_bool("decode.eos", d->eos_seen);
	UBYTE in_digest[16];
	char in_text[33];
	md5_final(&d->in_md5, in_digest);
	digest_text(in_digest, in_text);
	kv_str("input.md5", in_text);
	kv_unum("input.aus", d->sent_frames);
	const char *why = NULL;
	if (d->ref != NULL)
	{
		BOOL count_differs = d->frames != d->ref_count && d->max_frames == NO_LIMIT;
		kv_unum("ref.compared", d->frames < d->ref_count ? d->frames : d->ref_count);
		kv_unum("ref.mismatches", d->mismatches);
		kv_bool("ref.count_differs", count_differs);
		if (d->mismatches != 0)
			why = "frames differ from the reference";
		else if (count_differs)
			why = "the frame count differs from the reference";
	}
	if (d->flush_at != 0)
	{
		kv_unum("flush.frames", d->after_flush);
		kv_unum("flush.stale", d->stale);
		kv_unum("flush.untimed", d->untimed);
		if (why == NULL && (!d->flushed || d->after_flush == 0 || d->stale != 0 || d->untimed != 0))
			why = "the flush left stale or untimed frames, or no frames";
	}
	if (d->failed)
		return RETURN_FAIL;
	if (d->frames == 0)
		why = "no frames";
	if (why != NULL)
	{
		kv_str("fail", why);
		return RETURN_WARN;
	}
	return RETURN_OK;
}

/**
 * do_decode() - DECODE: the file through a native decoder session.
 * @t:    the tool
 * @args: decode_template's
 *
 * Return: RETURN_OK; RETURN_WARN when a check failed; RETURN_FAIL for arguments that make
 * no sense, a refused session or a failure; RETURN_ERROR when a file or memory is missing.
 */
int do_decode(struct tool *t, LONG *args)
{
	static struct decode d;
	memset(&d, 0, sizeof(d));
	const char *file = arg_str(args, ARG_FILE), *ref = arg_str(args, ARG_REF), *outname = arg_str(args, ARG_OUT);
	const char *outfmt_name = arg_str(args, ARG_OUTFORMAT);
	d.t = t;
	d.verbose = args[ARG_VERBOSE] != 0;
	d.hashes = args[ARG_HASHES] != 0;
	d.framed = args[ARG_FRAMED] != 0;
	d.nopts = args[ARG_NOPTS] != 0;
	d.norate = args[ARG_NORATE] != 0;
	d.fps = arg_num(args, ARG_FPS, 30u);
	d.max_frames = arg_num(args, ARG_FRAMES, NO_LIMIT);
	d.outfmt = outfmt_name != NULL ? format_of_name(outfmt_name) : VCF_I420;
	d.hash = args[ARG_NOHASH] == 0 && (d.outfmt == VCF_I420 || d.outfmt == VCF_NV12_COL128);
	d.flush_at = arg_num(args, ARG_FLUSH, 0);
	t->backend = arg_str(args, ARG_BACKEND);
	md5_init(&d.in_md5);
	ULONG codec = codec_of(arg_str(args, ARG_ENCODING), file);
	if (codec == VCC_None)
	{
		kv_str("fail", "ENCODING: H264, H265 or MJPEG");
		return RETURN_FAIL;
	}
	d.hevc = codec == VCC_H265;
	BOOL extradata = args[ARG_EXTRADATA] != 0;
	const char *config_name = arg_str(args, ARG_CONFIG);
	const char *tag_text = arg_str(args, ARG_TAG);
	ULONG codec_tag = tag_text != NULL ? tag_of_name(tag_text) : 0u;
	if (tag_text != NULL && codec_tag == 0)
	{
		kv_str("fail", "TAG: one to four characters");
		return RETURN_FAIL;
	}
	if (d.outfmt == 0 || d.fps == 0 || (d.flush_at != 0 && (!d.framed || ref != NULL)) ||
	    (extradata && (!d.framed || (codec != VCC_H264 && codec != VCC_H265) || config_name != NULL)))
	{
		kv_str("fail", "OUTFORMAT: I420, NV12_COL128, RGB16, RGB24, BGR24, BGRA32 or RGBA32; FPS: 1 or more; FLUSH needs FRAMED, and no REF; "
		               "EXTRADATA needs FRAMED and H.264 or H.265, and no CONFIG");
		return RETURN_FAIL;
	}
	kv_str("decode.file", file);
	kv_str("decode.codec", codec_name(codec));
	if (tag_text != NULL)
		kv_str("decode.tag", tag_text);
	kv_bool("decode.framed", d.framed);
	kv_bool("decode.hash", d.hash);
	kv_str("decode.out_format", format_name(d.outfmt));

	int rc = RETURN_ERROR;
	d.file = Open((CONST_STRPTR)file, MODE_OLDFILE);
	if (d.file == 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR)file);
		kv_str("fail", "the stream did not open");
		return RETURN_ERROR;
	}
	Seek(d.file, 0, OFFSET_END);
	LONG size = Seek(d.file, 0, OFFSET_BEGINNING);
	d.file_size = size > 0 ? (ULONG)size : 0;
	if (d.framed)
	{
		d.stream = size > 0 ? AllocVec((ULONG)size, MEMF_ANY) : NULL;
		if (d.stream == NULL || Read(d.file, d.stream, size) != size)
		{
			kv_str("fail", "cannot read the stream into memory");
			goto out_files;
		}
		d.stream_len = (ULONG)size;
		if (extradata && !split_config(&d, arg_num(args, ARG_PAD, 0)))
		{
			kv_str("fail", "EXTRADATA: no SPS or PPS before the first picture, or no memory");
			goto out_files;
		}
		if (extradata)
			kv_unum("decode.extradata", d.config_len);
	}
	if (config_name != NULL)
	{
		/* CONFIG: the file whole as the codec configuration */
		BPTR cf = Open((CONST_STRPTR)config_name, MODE_OLDFILE);
		LONG csize = -1;
		if (cf != 0)
		{
			Seek(cf, 0, OFFSET_END);
			csize = Seek(cf, 0, OFFSET_BEGINNING);
		}
		d.config = csize > 0 ? AllocVec((ULONG)csize, MEMF_ANY) : NULL;
		BOOL read_ok = d.config != NULL && Read(cf, d.config, csize) == csize;
		if (cf != 0)
			Close(cf);
		if (!read_ok)
		{
			kv_str("fail", "CONFIG: the file did not read");
			goto out_files;
		}
		d.config_len = (ULONG)csize;
		kv_unum("decode.extradata", d.config_len);
	}
	if (ref != NULL && !load_ref(&d, ref))
	{
		kv_str("fail", "the reference did not load");
		goto out_files;
	}
	if (outname != NULL)
	{
		d.outfile = Open((CONST_STRPTR)outname, MODE_NEWFILE);
		if (d.outfile == 0)
		{
			PrintFault(IoErr(), (CONST_STRPTR)outname);
			kv_str("fail", "the output file did not open");
			goto out_files;
		}
	}

	/* a native decoder session: the backend's own packets (an access unit mostly fits whole when framed; MJPEG takes a
	   whole picture per packet, so a JPEG file goes in one) */
	LONG err = 0;
	ULONG chunk = arg_num(args, ARG_PACKET, d.framed ? 262144u : codec == VCC_MJPEG && d.file_size > 65536u ? d.file_size : 65536u);
	ULONG rate = d.framed && !d.norate ? d.fps : 0u;
	d.s = VC_OpenSessionTags(VCA_Codec, codec, VCA_Kind, VCK_Decoder, VCA_SigTask, (ULONG)t->waiter.task, VCA_SigMask, waiter_sigmask(&t->waiter),
	                         VCA_Framed, (ULONG)d.framed, VCA_FrameRateNum, rate, VCA_FrameRateDen, 1, VCA_InputCount, DECODE_PACKETS,
	                         VCA_InputSize, chunk, VCA_OutputFormat, d.outfmt, BACKEND_TAG(t), VCA_Extradata, (ULONG)d.config,
	                         VCA_ExtradataSize, d.config_len, VCA_CodecTag, codec_tag, VCA_ErrorPtr, (ULONG)&err, TAG_DONE);
	kv_str("decode.session", d.s != NULL ? "open" : "refused");
	if (d.s == NULL)
	{
		kv_num("error", err);
		kv_str("fail", "the session was refused");
		rc = RETURN_FAIL;
		goto out_files;
	}
	u32 t_start = get_time();
	decode_loop(&d);
	rc = decode_summary(&d, get_time() - t_start);
	VC_CloseSession(d.s);
	d.s = NULL;
out_files:
	if (d.outfile != 0)
		Close(d.outfile);
	Close(d.file);
	if (d.ref != NULL)
		FreeVec(d.ref);
	if (d.stream != NULL)
		FreeVec(d.stream);
	if (d.config != NULL)
		FreeVec(d.config);
	return rc;
}
