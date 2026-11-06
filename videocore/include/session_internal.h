/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * session_internal.h - what the session sources share: the structures, the
 * kinds, and the functions one file calls in another.
 *
 *   videocore_session.c  the core: open and close, imports, sends and
 *                        releases, the poll and its events; it knows no kind
 *   session_objects.c    objects and sets: memory, attaching, sending; components opened and run; the input pictures
 *   session_formats.c    pixel formats, picture layout, rectangles, MMAL <-> client translation
 *   session_isp.c        the ISP: the output shape, its output side, the tunnel, geometry changes; the converter kind
 *   session_decoder.c    the decoder kind: ril.video_decode, native or tunnelled; the shape machinery the still shares
 *   session_still.c      the still kind: the image decoder's pipeline, one picture a session (the image calls' only)
 *   session_encoder.c    the encoder kind: H.264 (ril.video_encode), JPEG (ril.image_encode)
 *   h264_sps.c           the profile and pixel aspect from an H.264 stream's SPS
 *
 * A session has a kind, chosen at open from the client's parameters (enum
 * session_kind), and the kind's operations (struct session_ops): the core
 * calls them where the kinds differ and does the rest itself.  A decoder is
 * ril.video_decode, tunnelled into ril.isp when the client asked for an
 * output other than the decoder's native I420 picture (a pixel format, a
 * size, a place in the frame); a still is the image decoder's pipeline; a
 * converter is ril.isp alone; an encoder is ril.video_encode or
 * ril.image_encode.
 *
 * Each side holds a set of objects, the client's frames or packets.  Each
 * object carries its own MMAL buffer header over its memory, the backend's
 * own DMA allocation or the client's imported memory alike.  Once the side
 * starts, the objects are attached to its port (mmal_port_attach()), and a
 * buffer the VPU returns is its object.  A backend-owned input object
 * reaches the client through the poll before it is attached; the set is
 * attached at the first send, when it is complete.
 *
 * A decoder's or still's output starts with one token object attached, to
 * catch the stream's shape (FORMAT_CHANGED); the real set, or the ISP and
 * its set, come then (enum output_state says where the output is).  A still
 * decoded into the caller's own picture sends that picture first instead.
 * A decoder's codec configuration (VCA_Extradata) is an object of its own,
 * attached to the input port beside the set and sent first; the client
 * never sees it.
 *
 * Everything here runs under session_lock (the VCB_* entry points take it);
 * the mmal_* calls take mmal_lock themselves and may sleep.
 */
#ifndef SESSION_INTERNAL_H
#define SESSION_INTERNAL_H

#include <exec/types.h>
#include <exec/lists.h>

#include <types.h>
#include <bits.h>
#include <libraries/videocodec_backend.h>
#include <mmal.h>
#include <mmal_client.h>

struct VideoCoreBase;

#define SESSION_SET_MAX 16                /* objects on one side of a session */
#define SESSION_EVENTS_MAX 8              /* events waiting for the client's poll */
#define SESSION_MAGIC 0x56505553u         /* 'VPUS' */

_Static_assert(SESSION_SET_MAX + 1 <= CLIENT_PORT_BUFFERS_MAX, "a set and a decoder's codec configuration must fit on a port");

/* What a session is, from the client's VCK_* and codec (kind_of()) */
enum session_kind
{
	SESSION_DECODER,   /* a video decoder: ril.video_decode, native or tunnelled into ril.isp */
	SESSION_STILL,     /* a still picture's decoder: the image decoder's pipeline (the image calls' only) */
	SESSION_CONVERTER, /* ril.isp alone, pictures in and out */
	SESSION_ENCODER,   /* ril.video_encode or ril.image_encode */
};

/* Where the output frames are */
enum output_state
{
	OUTPUT_DOWN = 0,       /* no frames attached to a running output: before the shape is known, or while the output is remade */
	OUTPUT_START_PENDING,  /* the output starts at the next poll (after the stream's shape or a geometry change, so the client could set up its frames first) */
	OUTPUT_RUNNING,        /* the output set is attached to the running output port: releases go straight to the VPU */
	OUTPUT_CHANGE_WAITING, /* a new shape waits for the client's frames of the old one (session->format_change) */
};

/* A picture's layout in a buffer */
struct vc_geometry
{
	u32 planes;        /* 1 for a packed format, 3 for a planar one (Y, U, V) */
	u32 offset[3];     /* each plane's start in the buffer */
	u32 pitch[3];      /* each plane's bytes per row */
	u32 width, height; /* the buffer: pixels per row, rows */
	u32 size;          /* bytes of the whole picture */
};

/* One frame or packet of a side */
struct vc_object
{
	union
	{
		struct VCFrame frame;
		struct VCPacket packet;
	} pub;                     /* first: the client's pointer is the object's */
	struct mmal_buffer buffer; /* the MMAL buffer header over the memory (buffer.data): attached while its side runs */
	u32 capacity;              /* the memory's bytes */
	BOOL is_frame;             /* a picture, else a packet */
	BOOL owned;                /* the backend's memory */
	BOOL at_client;            /* in the client's hands */
	BOOL handed;               /* a backend-owned input object the client was given */
	u32 flushes;               /* the session's flush count at its last send: back with a picture from before a later flush, it is stale */
	u32 stamp;                 /* when it was last sent, or given to the client (profile tier) */
};

/* The objects of one side */
struct vc_set
{
	struct vc_object *objects[SESSION_SET_MAX];
	u32 count;
	u32 size;               /* the buffer size the port runs with */
	BOOL imported;          /* the client's memory, not the backend's */
	struct mmal_port *port; /* the port the objects are attached to, NULL while they are not; attached, the set is fixed */
};

/* The output frames' shape: the ISP's output, or the decoder's native picture */
struct vc_shape
{
	BOOL resolved;               /* the fields below are final: the tags gave the size, or the stream's shape came */
	ULONG format;                /* VCF_* */
	struct VCRect dest;          /* the picture's place in the frame */
	struct vc_geometry geometry; /* the frame's layout */
};

struct vc_session;

/*
 * A kind's operations, called by the core (videocore_session.c) where the
 * kinds differ.  A NULL entry means the kind has nothing to do there, or
 * does not take the call (VCERR_UNSUPPORTED).  Each file's table is below.
 */
struct session_ops
{
	/* Open the kind's components and make its own objects (session_open()) */
	LONG (*open)(struct vc_session *session);
	/* The first send: the sets attached, the ports running (session_start()) */
	LONG (*start)(struct vc_session *session);
	/* Before the objects go at the close: what the kind holds besides (a token) */
	void (*close)(struct vc_session *session);
	/* An input packet of the client's, checked and started: send it */
	LONG (*send_packet)(struct vc_session *session, struct vc_object *object, struct VCPacket *packet);
	/* An input frame of the client's, checked and started: send it */
	LONG (*send_frame)(struct vc_session *session, struct vc_object *object, struct VCFrame *frame);
	/* Describe a filled output object to the client as a poll item */
	void (*output_item)(struct vc_session *session, struct vc_object *object, struct VCItem *item);
	/* The token came back with a picture: TRUE if it is for the client (a still's frame), FALSE if its work is done */
	BOOL (*token_returned)(struct vc_session *session, struct vc_object *object);
	/* A FORMAT_CHANGED from the output port: the stream's shape, or a new one */
	void (*format_changed)(struct vc_session *session, const struct mmal_format_change *change);
	/* The client gave an output frame back */
	void (*frame_released)(struct vc_session *session);
	/* The output waits to start (OUTPUT_START_PENDING): start it */
	LONG (*output_start)(struct vc_session *session);
	/* Flush the kind's ports in the pipeline's order; NULL: the input and the running output */
	LONG (*flush)(struct vc_session *session);
	/* Change a running session (VC_SetAttrs()) */
	LONG (*set_attrs)(struct vc_session *session, const struct VCSessionParams *changes);
};

struct vc_session
{
	u32 magic;
	struct MinNode node; /* on the library base's sessions */
	struct VideoCoreBase *base;
	struct VCSessionParams params;
	enum session_kind kind;
	const struct session_ops *ops; /* the kind's */
	BOOL image_call;      /* one of an image call's: it may take a kept component, and the call reports the timings, not its close */
	BOOL started;         /* the first send: the sets are fixed, the ports run */
	BOOL failed;          /* over: the client hears why at its next poll */
	BOOL end_sent;        /* the client's end mark went out: the VPU's end of stream ends the stream (an empty one's ends at the send) */
	u32 flushes;          /* VC_Flush()es so far */
	struct mmal_component *decoder, *isp, *encoder;
	struct mmal_port *decoder_in, *decoder_out, *isp_in, *isp_out, *encoder_in, *encoder_out;
	struct mmal_port *input_port;  /* where input objects go */
	struct mmal_port *output_port; /* where output objects come back */
	struct vc_set input_set, output_set; /* each side's objects: the backend's own, or the client's imported */
	struct vc_geometry input_geometry; /* input pictures (encoder, converter) */
	struct VCStreamInfo stream; /* the stream's shape: a decoder's from FORMAT_CHANGED, a converter's input picture */
	BOOL have_stream;           /* stream is filled in */
	struct vc_shape shape;      /* the output frames' shape */
	enum output_state output_state;
	struct VCEvent events[SESSION_EVENTS_MAX]; /* events for the client's polls: a ring, the oldest at event_head */
	u32 event_head, event_count;
	/* the decoder's and the still's */
	BOOL tunnelled;          /* the decoder feeds ril.isp, or ril.isp the encoder, inside the VPU */
	struct vc_object *token; /* the output object that catches the stream's shape, until it is known */
	struct vc_object *config; /* the decoder's codec configuration (VCA_Extradata), sent first; never the client's */
	struct VCRational sps_par; /* the pixel aspect the H.264 SPS gives (the decoder reports 1/1); 0/0 for none */
	BOOL memory_checked;     /* the codec heap was read for a decoder that gives no picture size */
	BOOL sps_checked;        /* the stream's SPS was held against the codec heap (decoder_heap_check()) */
	u32 blind_bytes;         /* decoder input sent before the picture size came */
	struct mmal_format_change format_change; /* the shape that waits (OUTPUT_CHANGE_WAITING) */
};

/**
 * tag_count() - A set's object count from the client's tag, within 1..SESSION_SET_MAX.
 * @asked:    the tag's value, 0 when not given
 * @fallback: the backend's own count
 *
 * Return: the count.
 */
static inline u32 tag_count(ULONG asked, u32 fallback)
{
	return asked != 0 ? clamp_val(asked, 1, SESSION_SET_MAX) : fallback;
}

/**
 * param_output_format() - The output pixel format the client asked for.
 * @params: the session's parameters
 *
 * Return: VCA_OutputFormat, I420 when not given.
 */
static inline ULONG param_output_format(const struct VCSessionParams *params)
{
	return params->OutputFormat != 0 ? params->OutputFormat : VCF_I420;
}

/**
 * param_output_size_fixed() - Whether the client gave the output frames' size.
 * @params: the session's parameters
 *
 * Return: TRUE with both VCA_OutputWidth and VCA_OutputHeight, else the size follows the stream.
 */
static inline BOOL param_output_size_fixed(const struct VCSessionParams *params)
{
	return params->OutputWidth != 0 && params->OutputHeight != 0;
}

/**
 * output_running() - Whether the output set is attached to its running port.
 * @session: the session
 *
 * Return: TRUE while releases go straight to the VPU.
 */
static inline BOOL output_running(const struct vc_session *session)
{
	return session->output_state == OUTPUT_RUNNING;
}

/* The functions one file calls in another; those returning LONG give 0 or a VCERR_* */

/* videocore_session.c: the core */
/* Queue an event for the client's next poll */
void events_push(struct vc_session *session, ULONG event, LONG status);
/* End the session: the client learns why at its next poll */
void session_fail(struct vc_session *session, LONG error, const char *what);
/* Flush ports in order (NULL entries skipped) until one fails: a kind's flush */
LONG ports_flush(struct vc_session *session, struct mmal_port *const *ports, u32 count);

/* session_formats.c: pixel formats, picture layouts, rectangles, MMAL <-> client */
/* Whether a VCF_* format keeps luma and chroma in planes of their own */
BOOL format_is_planar(ULONG format);
#define COLUMN_BYTES 128u /* a column format's column: bytes per row of it */
#define COLUMN10_SAMPLES 96u /* the 10-bit column format's samples per 128-byte column row: three in 32 bits */
BOOL format_is_column(ULONG format);
/* Bytes per pixel of a VCF_* format's first plane, 1 for a planar one; 0 for an unknown format */
u32 format_bytes_per_pixel(ULONG format);
/* The MMAL encoding of a VCF_* pixel format; 0 for none */
ULONG format_encoding(ULONG format);
u32 color_space_mmal(ULONG space);
/* The encoding ril.video_decode's input takes for a VCC_* video codec (H.264, MJPEG); 0 for none */
ULONG video_codec_encoding(ULONG codec);
/* The encoding ril.image_decode's input takes for a VCC_* still picture's codec; 0 for none */
ULONG image_codec_encoding(ULONG codec);
/* A picture's layout in its buffer: planes, offsets, pitches, size; FALSE if it cannot be laid out */
BOOL picture_geometry(ULONG format, u32 width, u32 height, u32 pitch, struct vc_geometry *geometry);
/* Fill a video format: the encoding, the buffer's size, the visible picture, the frame rate */
void video_format_set(struct mmal_format *format, u32 encoding, u32 width, u32 height, const struct VCRect *crop,
                      const struct VCRational *rate);
/* The whole of a picture as a rectangle */
struct VCRect rect_whole(u32 width, u32 height);
/* Whether a rectangle is a non-empty part of a picture */
BOOL rect_inside(const struct VCRect *rect, u32 width, u32 height);
/* Whether two rectangles are the same */
BOOL rect_equal(const struct VCRect *first, const struct VCRect *second);
/* Describe a frame object to the client: its format, its layout, the visible picture */
void frame_set_geometry(struct vc_object *object, const struct vc_geometry *geometry, ULONG format, const struct VCRect *crop);
/* Describe a decoded native frame: the planes the VPU reported for it, else @geometry */
void frame_from_native(struct vc_object *object, const struct vc_geometry *geometry);
/* A returned frame's timestamp and flags, for the client */
void frame_from_return(struct vc_object *object);
/* A packet's VCPF_* flags as MMAL buffer flags */
ULONG packet_flags_to_mmal(ULONG flags);
/* A returned packet's length, timestamp and flags, for the client */
void packet_from_return(struct vc_object *object);

/* session_objects.c: objects and sets, their memory, attaching and sending; components; the input pictures */
/* A frame or packet object over the client's memory, or over the backend's own when @data is NULL */
struct vc_object *object_new(struct vc_session *session, BOOL is_frame, APTR data, u32 capacity);
/* Free an object, and its memory if it is the backend's */
void object_free(struct vc_session *session, struct vc_object *object);
/* The object behind a frame or packet pointer the client gave, on a side; NULL if none */
struct vc_object *object_lookup(struct vc_session *session, const void *pub, ULONG side, BOOL is_frame);
/* Send an object to the VPU through the port it is attached to */
LONG object_send(struct vc_session *session, struct vc_object *object, u32 length, ULONG flags, VCTime pts);
/* A new object, added to a set; NULL without memory or with the set full */
struct vc_object *set_new_object(struct vc_session *session, struct vc_set *set, BOOL is_frame, APTR data, u32 capacity);
/* Fill a set with @count objects of the backend's own memory, @size bytes each, in place of what it held */
LONG set_fill(struct vc_session *session, struct vc_set *set, BOOL is_frame, u32 count, u32 size);
/* Free a set's objects, leaving it open for new ones */
void set_empty(struct vc_session *session, struct vc_set *set);
/* Whether the client holds any object of a set */
BOOL set_at_client(const struct vc_set *set);
/* Attach a set's objects to a port, one buffer each, and one more of the backend's; the set is fixed from then on */
LONG set_attach(struct vc_session *session, struct vc_set *set, struct vc_object *extra, struct mmal_port *port);
/* An imported set's buffer size: its smallest object's */
void set_size_from_imports(struct vc_set *set);
/* Whether a set meets a port's minimum count and size; VCERR_BADPARAM, logged with @what, if not */
LONG set_check_minimum(const struct vc_set *set, const struct mmal_port *port, const char *what);
/* Size an imported set, check it against the port's minimum, attach it: a side at the first send */
LONG set_attach_checked(struct vc_session *session, struct vc_set *set, struct mmal_port *port, const char *what);
/* Stop a port and detach its set's objects (or the token) */
LONG set_detach(struct vc_session *session, struct mmal_port *port);
/* Send every output object the client does not hold to the VPU */
LONG set_send_output(struct vc_session *session, struct vc_set *set);
/* Give every frame of a set the same layout, format and visible picture */
void set_shape_frames(struct vc_set *set, const struct vc_geometry *geometry, ULONG format, const struct VCRect *crop);
/* How many output objects: the imported set's count, VCA_OutputCount, or @fallback */
u32 output_set_count(const struct vc_session *session, u32 fallback);
/* The output frames for session->shape: imported ones must hold @size, the backend's are made again */
LONG output_set_prepare(struct vc_session *session, u32 count, u32 size);
/* Describe a filled output frame as a poll item */
void output_item_frame(struct vc_object *object, struct VCItem *item);
/* Describe a filled output packet as a poll item */
void output_item_packet(struct vc_object *object, struct VCItem *item);
/* Create a VPU component for the session, or take the one an earlier image call kept, and take its first ports */
LONG component_open(struct vc_session *session, const char *name, struct mmal_component **component, struct mmal_port **in,
                    struct mmal_port **out);
/* Enable a component and its input and output ports, in that order */
LONG component_run(struct vc_session *session, struct mmal_component *component, struct mmal_port *in, struct mmal_port *out);
/* The input pictures' layout, from VCA_Width/Height, VCA_InputFormat and VCA_InputPitch; @isp_rows: planar rows as the ISP reads them */
LONG input_resolve(struct vc_session *session, BOOL isp_rows);
/* Describe an input picture to the client: the resolved layout, the whole picture visible */
void input_frame_shape(struct vc_session *session, struct vc_object *object);
/* The backend's own input pictures: VCA_InputCount of them, or two */
LONG input_set_fill(struct vc_session *session);
/* Commit the input pictures' format on a raw input port: an encoder's or the converter's */
LONG input_format_commit(struct vc_session *session, struct mmal_port *port);
/* Send an input picture of the client's: a converter's or encoder's send_frame */
LONG input_frame_send(struct vc_session *session, struct vc_object *object, struct VCFrame *frame);

/* session_isp.c: the ISP, the output shape, the tunnel, the converter kind */
extern const struct session_ops converter_ops;
/* The ISP's rows of a VCF_* format, in and out: whole groups of this many pixels */
u32 isp_row_pixels(ULONG format);
/* Whether a decoder's output needs the ISP: anything but the native I420 picture */
BOOL param_wants_isp(const struct VCSessionParams *params);
/* The output frames' shape: what the tags fix, the stream's shape for the rest */
LONG shape_resolve(struct vc_session *session);
/* Create the ISP (or take a kept one) and check it writes the output format */
LONG isp_open(struct vc_session *session);
/* A tunnel's ends take the stream's first shape; the ISP's output starts now or at the next poll */
LONG isp_tunnel_prepare(struct vc_session *session, const struct mmal_format_change *change);
/* Make the ISP's output side and run it: the whole tunnel at a decoder's first start, the port alone after a change */
LONG isp_output_start(struct vc_session *session);
/* A new output geometry for a session with the ISP (VC_SetAttrs()); a failure fails the session */
LONG isp_geometry_change(struct vc_session *session, const struct VCSessionParams *changes);

/* session_decoder.c: the decoder kind, and the shape machinery the still shares */
extern const struct session_ops decoder_ops;
/* The decoder's own output: a picture of a format and the visible part of it; FALSE for a size it cannot lay out */
BOOL shape_set_native(struct vc_session *session, ULONG format, u32 width, u32 height, u32 pitch, const struct VCRect *dest);
/* The decoder's output takes the stream's format, as a picture of @picture; the session learns the stream's shape */
LONG stream_format_take(struct vc_session *session, const struct mmal_format_change *change, ULONG picture);
/* The output stopped and bare, the token gone, the stream's shape taken: the start of a new shape */
LONG output_shape_begin(struct vc_session *session, const struct mmal_format_change *change, ULONG picture);
/* Commit the decoder's output for a picture format and buffer shape; the frames' layout follows the VPU's answer */
LONG native_output_commit(struct vc_session *session, const struct mmal_format_change *change, ULONG picture, u32 count, u32 size);
/* The output frames made, attached and sent: the native output runs */
LONG native_output_run(struct vc_session *session, u32 count, u32 size);
/* Whether a new shape has to wait for the client's frames (OUTPUT_CHANGE_WAITING then) */
BOOL output_change_waits(struct vc_session *session, const struct mmal_format_change *change);
/* A frame came home: apply a waiting shape once all are */
void output_change_release(struct vc_session *session);
/* A format change as a port's own format and needs (no FORMAT_CHANGED came) */
void format_change_from_port(const struct mmal_port *port, struct mmal_format_change *change);
/* The decoder output's token: one object of the backend's, attached, the port running, the token out */
LONG token_start(struct vc_session *session);
/* Free the token, if there is one (a decoder's or still's close) */
void token_free(struct vc_session *session);
/* The input set attached, the component and its input running: a decoder's or still's first send, before the output */
LONG decoder_input_start(struct vc_session *session);

/* session_still.c: the still kind */
extern const struct session_ops still_ops;
#define STILL_MAX_BYTES (126u << 20) /* a still's picture that goes by bulk: one transfer (vchiq.device's page-list slot, BCM2711) */
#define STILL_PACKET 81920u          /* the image decoder's input packets, its minimum: a picture's header must end in the first */

/* session_encoder.c: the encoder kind */
extern const struct session_ops encoder_ops;

#endif /* SESSION_INTERNAL_H */
