/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mmal_proto.h - the MMAL wire protocol over VCHIQ (the "mmal" service).
 *
 * The messages follow userland/interface/mmal/vc/mmal_vc_msgs.h (Copyright
 * (c) 2012, Broadcom Europe Ltd, BSD-3-Clause).  The peer is the 32-bit
 * little-endian ARM EABI client the firmware was written against: every
 * field is a 32-bit word (pointers included: the host hands out cookies and
 * the VPU echoes them), 64-bit timestamps are two words low first and
 * aligned to 8, so one structure carries explicit tail padding.  The layouts
 * below use 32-bit members only, which the 68k compiler packs like the ARM
 * one does, and their sizes are asserted.
 *
 * MMAL fourccs ('H264', 'I420', 'mmal') are little-endian style numbers,
 * MMAL_FOURCC() of mmal.h, and are swapped like every other word;
 * the VCHIQ service fourcc of the transport is the other convention.
 */
#ifndef MMAL_PROTO_H
#define MMAL_PROTO_H

#include <types.h>

/* The VCHIQ service the client opens, and the protocol versions it offers */
#define MMAL_SERVICE_FOURCC VCHIQ_FOURCC('m', 'm', 'a', 'l')
#define MMAL_SERVICE_VERSION 15     /* the version the client speaks */
#define MMAL_SERVICE_VERSION_MIN 10 /* the oldest it still works with */

#define MMAL_WIRE_MAGIC 0x6c616d6du /* every message's header.magic: MMAL_FOURCC('m','m','a','l') */

#define MMAL_WIRE_NAME_MAX 128      /* a component's name in COMPONENT_CREATE */
#define MMAL_WIRE_EXTRADATA_MAX 128 /* codec configuration bytes in a port info message */
#define MMAL_WIRE_SHORT_DATA 128    /* a buffer payload this short rides in the buffer message, not by bulk */
#define MMAL_WIRE_EVENT_SPACE 256   /* an event payload this short rides in the event message, not by bulk */
#define MMAL_WIRE_PARAM_WORDS 96    /* the most body words of a parameter set or get */
#define MMAL_WIRE_PARAM_MAX (MMAL_WIRE_PARAM_WORDS * 4u + 8u) /* the most bytes of a parameter, its id and size words included */
#define MMAL_WIRE_MAX_PORTS 8       /* the most ports of one type a component has */

/*
 * Message ids (MMAL_WORKER_CMD_T).  BUFFER_TO_HOST and EVENT_TO_HOST come
 * from the VPU unasked; every other id the host sends, and the VPU answers it
 * with the same id and the request's context.
 */
enum mmal_msgid
{
	MMAL_MSG_QUIT = 1,
	MMAL_MSG_SERVICE_CLOSED = 2,
	MMAL_MSG_GET_VERSION = 3,
	MMAL_MSG_COMPONENT_CREATE = 4,
	MMAL_MSG_COMPONENT_DESTROY = 5,
	MMAL_MSG_COMPONENT_ENABLE = 6,
	MMAL_MSG_COMPONENT_DISABLE = 7,
	MMAL_MSG_PORT_INFO_GET = 8,
	MMAL_MSG_PORT_INFO_SET = 9,
	MMAL_MSG_PORT_ACTION = 10,
	MMAL_MSG_BUFFER_FROM_HOST = 11,
	MMAL_MSG_BUFFER_TO_HOST = 12,
	MMAL_MSG_GET_STATS = 13,
	MMAL_MSG_PORT_PARAMETER_SET = 14,
	MMAL_MSG_PORT_PARAMETER_GET = 15,
	MMAL_MSG_EVENT_TO_HOST = 16,
	MMAL_MSG_GET_CORE_STATS_FOR_PORT = 17,
	MMAL_MSG_OPAQUE_ALLOCATOR = 18,
	MMAL_MSG_CONSUME_MEM = 19,
	MMAL_MSG_LMK = 20,
	MMAL_MSG_OPAQUE_ALLOCATOR_DESC = 21,
	MMAL_MSG_DRM_GET_LHS32 = 22,
	MMAL_MSG_DRM_GET_TIME = 23,
	MMAL_MSG_BUFFER_FROM_HOST_ZEROLEN = 24,
	MMAL_MSG_PORT_FLUSH = 25,
	MMAL_MSG_HOST_LOG = 26,
	MMAL_MSG_COMPACT = 27,
};

/* Port actions (MMAL_WORKER_PORT_ACTIONS), the action word of PORT_ACTION */
enum mmal_port_action
{
	MMAL_ACTION_ENABLE = 1,
	MMAL_ACTION_DISABLE = 2,
	MMAL_ACTION_FLUSH = 3,
	MMAL_ACTION_CONNECT = 4,
	MMAL_ACTION_DISCONNECT = 5,
	MMAL_ACTION_SET_REQUIREMENTS = 6,
};

/* Every message starts with this */
struct mmal_msg_header
{
	__le32 magic;           /* MMAL_WIRE_MAGIC */
	__le32 msgid;           /* enum mmal_msgid */
	__le32 control_service; /* unused by the host */
	__le32 context;         /* the host's cookie for the call, echoed in the reply */
	__le32 status;          /* MMAL status in replies that have no status field of their own */
	__le32 padding;
};

/* MMAL_PORT_T as the firmware lays it out; its pointers are the VPU's, never followed */
struct mmal_wire_port
{
	__le32 priv; /* pointer */
	__le32 name; /* pointer */
	__le32 type; /* MMAL_PORT_TYPE_* */
	__le16 index;     /* within its type */
	__le16 index_all; /* within the component */
	__le32 is_enabled;
	__le32 format; /* pointer: the format travels beside the port */
	__le32 buffer_num_min;
	__le32 buffer_size_min;
	__le32 buffer_alignment_min;
	__le32 buffer_num_recommended;
	__le32 buffer_size_recommended;
	__le32 buffer_num;
	__le32 buffer_size;
	__le32 component; /* pointer */
	__le32 userdata;  /* pointer */
	__le32 capabilities; /* MMAL_PORT_CAPABILITY_* */
};

/*
 * MMAL_ES_FORMAT_T; its es and extradata are pointers on the VPU's side, sent
 * as 0: the parts themselves follow it in the message (the type-specific
 * words, then the extradata bytes)
 */
struct mmal_wire_es_format
{
	__le32 type;     /* MMAL_ES_TYPE_* */
	__le32 encoding; /* a fourcc */
	__le32 encoding_variant;
	__le32 es; /* pointer */
	__le32 bitrate;
	__le32 flags;
	__le32 extradata_size;
	__le32 extradata; /* pointer */
};

/* MMAL_ES_SPECIFIC_FORMAT_T: a union sized by its video member (size, crop, rate, aspect, colour space); 11 words either way */
#define MMAL_WIRE_ES_WORDS 11
struct mmal_wire_es_specific
{
	__le32 words[MMAL_WIRE_ES_WORDS];
};

/* GET_VERSION and its reply: the request carries only its header, the reply the VPU's MMAL version */
struct mmal_msg_version
{
	struct mmal_msg_header h;
	__le32 flags;
	__le32 major; /* decides how flushes and end-of-stream markers travel */
	__le32 minor;
	__le32 minimum;
};

/* COMPONENT_CREATE */
struct mmal_msg_component_create
{
	struct mmal_msg_header h;
	__le32 client_component; /* the host's cookie, echoed in events */
	u8 name[MMAL_WIRE_NAME_MAX]; /* e.g. "ril.video_decode" */
	__le32 pid; /* 0 */
};

/* Its reply: the VPU's handle, and the component's ports of each type */
struct mmal_msg_component_create_reply
{
	struct mmal_msg_header h;
	__le32 status;
	__le32 component_handle;
	__le32 input_num;
	__le32 output_num;
	__le32 clock_num;
};

/* COMPONENT_DESTROY, COMPONENT_ENABLE, COMPONENT_DISABLE: only the component's handle */
struct mmal_msg_component_handle
{
	struct mmal_msg_header h;
	__le32 component_handle;
};

/* A reply that is only a status: component destroy and enable, port actions, parameter sets, flushes */
struct mmal_msg_reply
{
	struct mmal_msg_header h;
	__le32 status;
};

/* PORT_INFO_GET: a port by its type and its index within the type */
struct mmal_msg_port_info_get
{
	struct mmal_msg_header h;
	__le32 component_handle;
	__le32 port_type; /* MMAL_PORT_TYPE_* */
	__le32 index;
};

/* PORT_INFO_SET: the port's buffer shape and its format, as the host wants them */
struct mmal_msg_port_info_set
{
	struct mmal_msg_header h;
	__le32 component_handle;
	__le32 port_type;
	__le32 index;
	struct mmal_wire_port port;
	struct mmal_wire_es_format format;
	struct mmal_wire_es_specific es;
	u8 extradata[MMAL_WIRE_EXTRADATA_MAX];
};

/* The reply to both port info messages: the port as the VPU now has it */
struct mmal_msg_port_info
{
	struct mmal_msg_header h;
	__le32 status;
	__le32 component_handle;
	__le32 port_type;
	__le32 index;
	__le32 found;
	__le32 port_handle; /* the VPU's, for the port's actions, parameters and buffers */
	struct mmal_wire_port port;
	struct mmal_wire_es_format format;
	struct mmal_wire_es_specific es;
	u8 extradata[MMAL_WIRE_EXTRADATA_MAX];
};

/* PORT_ACTION; the argument depends on the action, and is 0 for the rest */
struct mmal_msg_port_action
{
	struct mmal_msg_header h;
	__le32 component_handle;
	__le32 port_handle;
	__le32 action; /* enum mmal_port_action */
	union
	{
		struct mmal_wire_port port; /* enable, set requirements */
		struct
		{
			__le32 component_handle;
			__le32 port_handle;
		} connect; /* connect: the input port the output feeds */
	} param;
};

/*
 * PORT_PARAMETER_SET and PORT_PARAMETER_GET: the parameter is its id, its size
 * and its body.  The message ends with the parameter: it is
 * MMAL_MSG_PORT_PARAM_FIXED + size bytes, not the whole structure.  A get
 * sends a body as long as its room for the answer: the size tells the VPU
 * how much will fit.
 */
struct mmal_msg_port_param
{
	struct mmal_msg_header h;
	__le32 component_handle;
	__le32 port_handle;
	__le32 id;   /* MMAL_PARAMETER_* */
	__le32 size; /* bytes of the parameter: the id and size words and the body */
	__le32 space[MMAL_WIRE_PARAM_WORDS]; /* the body */
};
#define MMAL_MSG_PORT_PARAM_FIXED 32u /* bytes before the parameter */

/* The reply to a get: the parameter as the VPU fills it; with ENOSPC, size states what would fit the answer */
struct mmal_msg_port_param_get_reply
{
	struct mmal_msg_header h;
	__le32 status;
	__le32 id;
	__le32 size; /* bytes of the whole answer, the id and size words included */
	__le32 space[MMAL_WIRE_PARAM_WORDS];
};
#define MMAL_MSG_PORT_PARAM_REPLY_FIXED 28u /* bytes before the parameter */

/* MMAL_DRIVER_BUFFER_T: the buffer's identity on both sides */
struct mmal_wire_driver_buffer
{
	__le32 magic; /* MMAL_WIRE_MAGIC */
	__le32 component_handle;
	__le32 port_handle;
	__le32 client_context; /* the host's cookie, echoed when the buffer comes back */
};

/* MMAL_BUFFER_HEADER_T; its pointers are the VPU's, never followed */
struct mmal_wire_buffer_header
{
	__le32 next; /* pointer */
	__le32 priv; /* pointer */
	__le32 cmd;  /* 0 for data, else the event's fourcc */
	__le32 data; /* the host's address; the payload itself travels inline or by bulk */
	__le32 alloc_size;
	__le32 length; /* valid bytes from data + offset */
	__le32 offset; /* 0 from the host */
	__le32 flags;  /* MMAL_BUFFER_HEADER_FLAG_* */
	__le32 pts_lo; /* microseconds, or MMAL_TIME_UNKNOWN */
	__le32 pts_hi;
	__le32 dts_lo; /* likewise; the host sends it unknown */
	__le32 dts_hi;
	__le32 type;      /* pointer: the video part travels beside the header */
	__le32 user_data; /* pointer */
};

/* MMAL_BUFFER_HEADER_TYPE_SPECIFIC_T, its video member: the planes of a picture in the buffer */
struct mmal_wire_video_specific
{
	__le32 planes; /* 0 when the component did not describe them */
	__le32 offset[4];
	__le32 pitch[4];
	__le32 flags; /* MMAL_BUFFER_HEADER_VIDEO_FLAG_* */
};

/* BUFFER_FROM_HOST, BUFFER_FROM_HOST_ZEROLEN, PORT_FLUSH, and BUFFER_TO_HOST back */
struct mmal_msg_buffer
{
	struct mmal_msg_header h;
	struct mmal_wire_driver_buffer drvbuf;
	struct mmal_wire_driver_buffer drvbuf_ref; /* unused: 0 */
	struct mmal_wire_buffer_header buffer_header;
	struct mmal_wire_video_specific video;
	__le32 is_zero_copy;  /* 0: the payload goes inline or by bulk */
	__le32 has_reference; /* 0 */
	__le32 payload_in_message; /* bytes of short_data that carry the payload, 0 when it goes by bulk */
	u8 short_data[MMAL_WIRE_SHORT_DATA];
	u8 pad[4]; /* the ARM structure is padded to a multiple of 8 */
};

/* EVENT_TO_HOST: an event of a component's port, its payload inline or (longer) by bulk */
struct mmal_msg_event
{
	struct mmal_msg_header h;
	__le32 client_component; /* the host's cookie from COMPONENT_CREATE */
	__le32 port_type;        /* MMAL_PORT_TYPE_* */
	__le32 port_num;         /* within its type */
	__le32 cmd;              /* MMAL_EVENT_* */
	__le32 length;           /* payload bytes: in data up to MMAL_WIRE_EVENT_SPACE, by bulk above */
	u8 data[MMAL_WIRE_EVENT_SPACE];
	__le32 delayed_buffer; /* pointer */
};

/* FORMAT_CHANGED event payload: MMAL_EVENT_FORMAT_CHANGED_T, then the format, its es, then extradata */
struct mmal_wire_format_changed
{
	__le32 buffer_size_min;
	__le32 buffer_num_min;
	__le32 buffer_size_recommended;
	__le32 buffer_num_recommended;
	__le32 format; /* a VPU pointer: ignored */
	struct mmal_wire_es_format es_format;
	struct mmal_wire_es_specific es;
};

/* The firmware's sizes: a mismatch is a layout mistake above */
_Static_assert(sizeof(struct mmal_msg_header) == 24, "msg header layout");
_Static_assert(sizeof(struct mmal_wire_port) == 64, "port layout");
_Static_assert(sizeof(struct mmal_wire_es_format) == 32, "es format layout");
_Static_assert(sizeof(struct mmal_wire_es_specific) == 44, "es specific layout");
_Static_assert(sizeof(struct mmal_msg_version) == 40, "version layout");
_Static_assert(sizeof(struct mmal_msg_component_create) == 160, "component create layout");
_Static_assert(sizeof(struct mmal_msg_component_create_reply) == 44, "component create reply layout");
_Static_assert(sizeof(struct mmal_msg_component_handle) == 28, "component handle layout");
_Static_assert(sizeof(struct mmal_msg_reply) == 28, "reply layout");
_Static_assert(sizeof(struct mmal_msg_port_info_get) == 36, "port info get layout");
_Static_assert(sizeof(struct mmal_msg_port_info_set) == 304, "port info set layout");
_Static_assert(sizeof(struct mmal_msg_port_info) == 316, "port info layout");
_Static_assert(sizeof(struct mmal_msg_port_action) == 100, "port action layout");
_Static_assert(sizeof(struct mmal_msg_port_param) == 424, "port param layout");
_Static_assert(sizeof(struct mmal_msg_port_param_get_reply) == 420, "port param get reply layout");
_Static_assert(sizeof(struct mmal_wire_driver_buffer) == 16, "driver buffer layout");
_Static_assert(sizeof(struct mmal_wire_buffer_header) == 56, "buffer header layout");
_Static_assert(sizeof(struct mmal_wire_video_specific) == 40, "video specific layout");
_Static_assert(sizeof(struct mmal_msg_buffer) == 296, "buffer message layout");
_Static_assert(sizeof(struct mmal_msg_event) == 304, "event message layout");
_Static_assert(sizeof(struct mmal_wire_format_changed) == 96, "format changed layout");

#endif /* MMAL_PROTO_H */
