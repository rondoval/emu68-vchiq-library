/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * libraries/videocodec_backend.h - what a videocodec.library backend is.
 *
 * A backend is an Exec library named <name>.codec in LIBS:videocodec/.  Its
 * function table is sfd/videocodec_backend.sfd: VCB_* calls, arguments in
 * registers, the library base in A6.  Ship the file in the drawer and
 * nothing else; videocodec.library finds it.
 *
 *
 * How the frontend uses a backend
 *
 * - At its first use by a Process, the frontend opens every file in the
 *   drawer whose name ends in .codec (any case; icons and other files are
 *   left alone).  It keeps the backend open until its own expunge.
 * - When its last opener closes it, the frontend calls VCB_Idle(), in that
 *   opener's task: what the backend keeps between calls for speed (a
 *   connection to the hardware, components set up) goes, and the next call
 *   makes it again.  The backend may wait for its hardware
 *   there, which its expunge must not.
 * - It calls VCB_GetInfo() first.  A backend is kept only if it reports the
 *   ABI major of this header and VCInfo.Codecs, the number of rows
 *   VCB_QueryCodec() answers (index 0 to Codecs - 1).  The table is read
 *   once and cached.  VCCodecInfo.ChainOutputFormats is the frontend's to
 *   fill: a backend leaves it 0.
 * - A session goes to the backend whose row for its codec and kind serves
 *   sessions (VCCF_Session) and has the highest VCCodecInfo.Priority
 *   (VCB_PRIORITY_*; ties by backend name), or to the one the program names
 *   with VCA_Backend.  An image call (VC_DecodeImage(), VC_EncodeImage(),
 *   VC_ConvertImage()) goes the same way to a row that serves image calls
 *   (VCCF_Image), as one VCB_Image().
 * - The frontend parses the tags.  The backend gets a struct
 *   VCSessionParams or VCImageParams and never sees a tag.  Every other
 *   call is forwarded one to one.
 *
 *
 * What a backend promises
 *
 * - Every structure crossing this interface starts with Size and grows only
 *   at its end.  A reader never reads past the smaller of its own sizeof and
 *   the peer's Size.  New functions go at the end of the table.
 * - The objects (struct VCFrame, struct VCPacket, see libraries/videocodec.h)
 *   are the backend's: it allocates them with the public part first and its
 *   own data behind, hands them to the client through the frontend, and
 *   frees them with the session.
 * - Ownership, order and signalling are as libraries/videocodec.h promises
 *   the client.
 * - Errors are VCERR_*.
 * - Any task may call a backend at any time; the backend serialises itself.
 * - VCB_OutputFrame() (ABI 1.1) lists a decoder session's output frames:
 *   the VCFrame objects the backend made to decode pictures into, which
 *   VCB_Poll() otherwise hands out one at a time as pictures finish.  The
 *   frontend calls it with 0, 1, 2 and so on, and stops at the first NULL;
 *   each answer is one of those frames, the same object whenever it is asked.
 *   The index only walks the set: it is no picture number.  The frames exist
 *   once the stream's size is known (an open that gave the size or the
 *   extradata, else after its VCEV_StreamInfo); until then the answer is
 *   NULL, and a new size brings new frames, listed again after its
 *   VCEV_StreamInfo.  The frontend needs the list to put a converter behind
 *   a decoder whose output format the client cannot take: a converter takes
 *   its input frames by import before its first picture only, so every one
 *   of the decoder's frames is imported up front, and each decoded picture
 *   then reaches the converter where it lies, with no copy.  A backend of
 *   ABI 1.0 lacks the function; its decoders get no converter.
 */
#ifndef LIBRARIES_VIDEOCODEC_BACKEND_H
#define LIBRARIES_VIDEOCODEC_BACKEND_H

#include <libraries/videocodec.h>

/*
 * The ABI version a backend reports in VCInfo.ABIVersion: major << 16 | minor.
 * The frontend keeps a backend only if the major matches its own.  A minor
 * step will add fields at the end of a structure or functions at the end of
 * the table, so either side reads a newer peer as far as its own sizeof and
 * an older one as far as the peer's Size.  1.0 is the first.
 */
#define VCB_ABI_VERSION_MAJOR 1
#define VCB_ABI_VERSION_MINOR 1
#define VCB_ABI_VERSION ((VCB_ABI_VERSION_MAJOR << 16) | VCB_ABI_VERSION_MINOR)
#define VCB_ABI_MAJOR(v) ((v) >> 16)
#define VCB_ABI_MINOR(v) ((v) & 0xffffu)

/*
 * VCCodecInfo.Priority, set by the backend in every row it returns: which
 * backend gets a session when several have the codec.  Per row, so a
 * backend can be hardware for one codec and software for another.  Higher
 * wins, ties by backend name; Exec's range, -128 to 127.  A row that is
 * faster than software but not dedicated hardware goes in between; one that
 * must win over the hardware for a reason of its own goes above.
 */
#define VCB_PRIORITY_HARDWARE 50 /* the codec runs on dedicated hardware */
#define VCB_PRIORITY_SOFTWARE 0  /* the 68k does the work */

/*
 * VCSessionParams.Changed: what a VCB_SetAttrs() carries (0 at open:
 * everything).  A SigTask or SigMask set in a VCB_SetAttrs() is signalled
 * from then on, whatever Changed says.  An output geometry change merges
 * into what the session has: a field given replaces the one kept, 0 keeps
 * it; a new format or width without a pitch drops the pitch kept;
 * HaveDestRect and KeepAspect replace each other.  The frontend reopens a
 * converter behind a decoder with what it merged by the same rule, at each
 * new stream shape.
 */
#define VCSPF_Bitrate (1UL << 0)
#define VCSPF_RequestKeyFrame (1UL << 1)
#define VCSPF_OutputGeometry (1UL << 2) /* OutputFormat/Width/Height/Pitch, DestRect, KeepAspect: the output set is remade */

/*
 * A session's tags, parsed (VCB_OpenSession(), VCB_SetAttrs()).  Each field
 * is the tag of its name in libraries/videocodec.h, which says what it means
 * and which kinds read it.  0 means not given: the backend's default.
 */
struct VCSessionParams
{
	ULONG Size;
	ULONG Changed;                       /* VCSPF_*: what a VCB_SetAttrs() carries; 0 at open */
	ULONG Codec, Kind;                   /* VCC_*, VCK_* */
	ULONG Width, Height;                 /* input pictures; a decoder: what imported native frames are checked against */
	ULONG InputFormat, InputPitch;       /* input pictures: VCF_*, bytes per row */
	ULONG OutputFormat;                  /* output pictures: VCF_* */
	ULONG OutputWidth, OutputHeight;     /* output pictures: pixels, rows */
	ULONG OutputPitch;                   /* output pictures: bytes per row */
	struct VCRect DestRect;              /* where the picture goes in an output frame */
	ULONG HaveDestRect;                  /* DestRect was given */
	struct VCRect SourceRect;            /* a converter: the part of the input picture to take */
	ULONG HaveSourceRect;                /* SourceRect was given */
	ULONG KeepAspect;                    /* fit the picture in the output frame, centred, instead of stretching it */
	ULONG InputCount, InputSize;         /* the backend's own input buffers: how many, and a packet's bytes */
	ULONG OutputCount, OutputSize;       /* the backend's own output buffers: how many, and a packet's bytes */
	struct Task *SigTask;                /* the task to signal: the opener when a mask came without one; VCB_SetAttrs(): another, when set */
	ULONG SigMask;                       /* the signal bits to Signal() when something is ready; 0: the client polls; VCB_SetAttrs(): others, when set */
	ULONG Framed;                        /* a decoder: each packet holds whole pictures */
	struct VCRational FrameRate;         /* the input's frame rate; Num 0 when not given */
	ULONG Bitrate, Profile, Level;       /* an encoder: bits per second, VCP_*, H.264 level times ten */
	ULONG KeyInterval, RateControl;      /* an encoder: pictures between key frames, VCRC_* */
	ULONG Quality;                       /* a JPEG encoder: 1 to 100 */
	ULONG RequestKeyFrame;               /* an encoder, VCB_SetAttrs() only: make the next picture a key frame */
	ULONG ExtradataSize;                 /* bytes at Extradata */
	CONST_APTR Extradata;                /* a decoder: the codec configuration (H.264 SPS/PPS); valid in VCB_OpenSession() only */
	ULONG ColorSpace;                    /* an encoder: VCCS_* of its input pictures */
	ULONG CodecTag;                      /* VCA_CodecTag: the container's FourCC for the codec, 0 none (ABI 1.1) */
};

/*
 * An image call (VCB_Image()): one picture, blocking, in the caller's task.
 * The kind says what each side is; a picture side's fields come from the
 * VCA_Width/Height/InputFormat/InputPitch/SourceRect tags (the source) or
 * the VCA_Output* and VCA_DestRect tags (the destination).
 *
 *               Source             Dest
 *   decoder     compressed bytes   a picture
 *   encoder     a picture          room for compressed bytes
 *   converter   a picture          a picture
 */
struct VCImageSide
{
	APTR Data;           /* the bytes or the picture, in the caller's memory */
	ULONG Length;        /* compressed: the source's bytes, or the destination's room */
	ULONG Format;        /* a picture: VCF_* */
	ULONG Width, Height; /* a picture: pixels; a destination's 0x0 is the source's size */
	ULONG Pitch;         /* a picture: bytes per row of its first plane, 0 for the natural pitch */
	struct VCRect Rect;  /* a picture: the source's part to take, the destination's place to fill */
	ULONG HaveRect;
};

struct VCImageParams
{
	ULONG Size;
	ULONG Kind;  /* VCK_* */
	ULONG Codec; /* VCC_*: the decoder's or encoder's; VCC_Raw for a converter */
	struct VCImageSide Source, Dest;
	ULONG KeepAspect; /* the picture keeps its aspect inside the destination's place */
	ULONG Quality;    /* an encoder's, 1 to 100; 0 for the backend's default */
	ULONG TimeoutMs;  /* how long each pass may wait for the hardware; 0 for the backend's default */
	ULONG Written;    /* set by the backend: the bytes written at Dest.Data (an encoder) */
};

#endif /* LIBRARIES_VIDEOCODEC_BACKEND_H */
