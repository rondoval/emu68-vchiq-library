/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mmal.h - MMAL, the VPU firmware's multimedia interface, as the MMAL client
 * (mmal_client.h) and the session layer use it: status codes, port types,
 * encodings, buffer flags, events, parameter ids, and the elementary stream
 * format.  The values are Broadcom's (the headers in userland/interface/mmal,
 * BSD-3-Clause): a selection, somewhat more than this backend uses.  The
 * parameter ids are numbered by their place in the enums: none may be
 * removed or moved.
 */
#ifndef VIDEOCORE_MMAL_H
#define VIDEOCORE_MMAL_H

#include <exec/types.h>

#include <types.h>

/* MMAL fourccs are little-endian style numbers, as in Broadcom's headers. */
#define MMAL_FOURCC(a, b, c, d) ((ULONG)(a) | ((ULONG)(b) << 8) | ((ULONG)(c) << 16) | ((ULONG)(d) << 24))

#define MMAL_TIME_UNKNOWN ((s64)1 << 63) /* no timestamp (timestamps are s64 microseconds) */

/* Status codes, as the VPU reports them (the client turns them into VCERR_*) */
#define MMAL_SUCCESS 0
#define MMAL_ENOMEM 1
#define MMAL_ENOSPC 2
#define MMAL_EINVAL 3
#define MMAL_ENOSYS 4
#define MMAL_ENOENT 5
#define MMAL_ENXIO 6
#define MMAL_EIO 7
#define MMAL_ESPIPE 8
#define MMAL_ECORRUPT 9
#define MMAL_ENOTREADY 10
#define MMAL_ECONFIG 11
#define MMAL_EISCONN 12
#define MMAL_ENOTCONN 13
#define MMAL_EAGAIN 14
#define MMAL_EFAULT 15

/* Port types (mmal_port.type) */
#define MMAL_PORT_TYPE_CONTROL 1
#define MMAL_PORT_TYPE_INPUT 2
#define MMAL_PORT_TYPE_OUTPUT 3
#define MMAL_PORT_TYPE_CLOCK 4

/* Port capabilities (mmal_port.capabilities) */
#define MMAL_PORT_CAPABILITY_PASSTHROUGH 0x01
#define MMAL_PORT_CAPABILITY_ALLOCATION 0x02
#define MMAL_PORT_CAPABILITY_SUPPORTS_EVENT_FORMAT_CHANGE 0x04

/* Elementary stream types (mmal_format.type) */
#define MMAL_ES_TYPE_UNKNOWN 0
#define MMAL_ES_TYPE_CONTROL 1
#define MMAL_ES_TYPE_AUDIO 2
#define MMAL_ES_TYPE_VIDEO 3
#define MMAL_ES_TYPE_SUBPICTURE 4

/* Format flags (mmal_format.flags) */
#define MMAL_ES_FORMAT_FLAG_FRAMED 0x1 /* every buffer holds whole frames: no parsing needed */

/* Encodings (mmal_format.encoding): a selection of the pre-defined ones */
#define MMAL_ENCODING_UNKNOWN 0
#define MMAL_ENCODING_H264 MMAL_FOURCC('H', '2', '6', '4')
#define MMAL_ENCODING_MVC MMAL_FOURCC('M', 'V', 'C', ' ')
#define MMAL_ENCODING_H263 MMAL_FOURCC('H', '2', '6', '3')
#define MMAL_ENCODING_MP4V MMAL_FOURCC('M', 'P', '4', 'V')
#define MMAL_ENCODING_MP2V MMAL_FOURCC('M', 'P', '2', 'V')
#define MMAL_ENCODING_MP1V MMAL_FOURCC('M', 'P', '1', 'V')
#define MMAL_ENCODING_WMV3 MMAL_FOURCC('W', 'M', 'V', '3')
#define MMAL_ENCODING_WMV2 MMAL_FOURCC('W', 'M', 'V', '2')
#define MMAL_ENCODING_WMV1 MMAL_FOURCC('W', 'M', 'V', '1')
#define MMAL_ENCODING_WVC1 MMAL_FOURCC('W', 'V', 'C', '1')
#define MMAL_ENCODING_VP8 MMAL_FOURCC('V', 'P', '8', ' ')
#define MMAL_ENCODING_VP7 MMAL_FOURCC('V', 'P', '7', ' ')
#define MMAL_ENCODING_VP6 MMAL_FOURCC('V', 'P', '6', ' ')
#define MMAL_ENCODING_THEORA MMAL_FOURCC('T', 'H', 'E', 'O')
#define MMAL_ENCODING_SPARK MMAL_FOURCC('S', 'P', 'R', 'K')
#define MMAL_ENCODING_MJPEG MMAL_FOURCC('M', 'J', 'P', 'G')
#define MMAL_ENCODING_JPEG MMAL_FOURCC('J', 'P', 'E', 'G')
#define MMAL_ENCODING_GIF MMAL_FOURCC('G', 'I', 'F', ' ')
#define MMAL_ENCODING_PNG MMAL_FOURCC('P', 'N', 'G', ' ')
#define MMAL_ENCODING_PPM MMAL_FOURCC('P', 'P', 'M', ' ')
#define MMAL_ENCODING_TGA MMAL_FOURCC('T', 'G', 'A', ' ')
#define MMAL_ENCODING_BMP MMAL_FOURCC('B', 'M', 'P', ' ')
#define MMAL_ENCODING_I420 MMAL_FOURCC('I', '4', '2', '0')
#define MMAL_ENCODING_I420_SLICE MMAL_FOURCC('S', '4', '2', '0')
#define MMAL_ENCODING_YV12 MMAL_FOURCC('Y', 'V', '1', '2')
#define MMAL_ENCODING_I422 MMAL_FOURCC('I', '4', '2', '2')
#define MMAL_ENCODING_I422_SLICE MMAL_FOURCC('S', '4', '2', '2')
#define MMAL_ENCODING_YUYV MMAL_FOURCC('Y', 'U', 'Y', 'V')
#define MMAL_ENCODING_YVYU MMAL_FOURCC('Y', 'V', 'Y', 'U')
#define MMAL_ENCODING_UYVY MMAL_FOURCC('U', 'Y', 'V', 'Y')
#define MMAL_ENCODING_VYUY MMAL_FOURCC('V', 'Y', 'U', 'Y')
#define MMAL_ENCODING_NV12 MMAL_FOURCC('N', 'V', '1', '2')
#define MMAL_ENCODING_NV21 MMAL_FOURCC('N', 'V', '2', '1')
#define MMAL_ENCODING_ARGB MMAL_FOURCC('A', 'R', 'G', 'B')
#define MMAL_ENCODING_ARGB_SLICE MMAL_FOURCC('a', 'r', 'g', 'b')
#define MMAL_ENCODING_RGBA MMAL_FOURCC('R', 'G', 'B', 'A')
#define MMAL_ENCODING_RGBA_SLICE MMAL_FOURCC('r', 'g', 'b', 'a')
#define MMAL_ENCODING_ABGR MMAL_FOURCC('A', 'B', 'G', 'R')
#define MMAL_ENCODING_ABGR_SLICE MMAL_FOURCC('a', 'b', 'g', 'r')
#define MMAL_ENCODING_BGRA MMAL_FOURCC('B', 'G', 'R', 'A')
#define MMAL_ENCODING_BGRA_SLICE MMAL_FOURCC('b', 'g', 'r', 'a')
#define MMAL_ENCODING_RGB16 MMAL_FOURCC('R', 'G', 'B', '2')
#define MMAL_ENCODING_RGB16_SLICE MMAL_FOURCC('r', 'g', 'b', '2')
#define MMAL_ENCODING_RGB24 MMAL_FOURCC('R', 'G', 'B', '3')
#define MMAL_ENCODING_RGB24_SLICE MMAL_FOURCC('r', 'g', 'b', '3')
#define MMAL_ENCODING_RGB32 MMAL_FOURCC('R', 'G', 'B', '4')
#define MMAL_ENCODING_RGB32_SLICE MMAL_FOURCC('r', 'g', 'b', '4')
#define MMAL_ENCODING_BGR16 MMAL_FOURCC('B', 'G', 'R', '2')
#define MMAL_ENCODING_BGR16_SLICE MMAL_FOURCC('b', 'g', 'r', '2')
#define MMAL_ENCODING_BGR24 MMAL_FOURCC('B', 'G', 'R', '3')
#define MMAL_ENCODING_BGR24_SLICE MMAL_FOURCC('b', 'g', 'r', '3')
#define MMAL_ENCODING_BGR32 MMAL_FOURCC('B', 'G', 'R', '4')
#define MMAL_ENCODING_BGR32_SLICE MMAL_FOURCC('b', 'g', 'r', '4')
#define MMAL_ENCODING_YUVUV128 MMAL_FOURCC('S', 'A', 'N', 'D')
#define MMAL_ENCODING_OPAQUE MMAL_FOURCC('O', 'P', 'Q', 'V')
#define MMAL_ENCODING_PCM_SIGNED_LE MMAL_FOURCC('p', 'c', 'm', 's')
#define MMAL_ENCODING_PCM_SIGNED_BE MMAL_FOURCC('P', 'C', 'M', 'S')
#define MMAL_ENCODING_VARIANT_DEFAULT 0
#define MMAL_ENCODING_VARIANT_H264_DEFAULT 0
#define MMAL_ENCODING_VARIANT_H264_AVC1 MMAL_FOURCC('A', 'V', 'C', '1') /* length-prefixed NALs, config in extradata */
#define MMAL_ENCODING_VARIANT_H264_RAW MMAL_FOURCC('R', 'A', 'W', ' ')  /* Annex-B start codes */
#define MMAL_ENCODING_VARIANT_MP4A_DEFAULT 0

/* Colour spaces (mmal_format.es.video.color_space) */
#define MMAL_COLOR_SPACE_UNKNOWN 0
#define MMAL_COLOR_SPACE_ITUR_BT601 MMAL_FOURCC('Y', '6', '0', '1')
#define MMAL_COLOR_SPACE_ITUR_BT709 MMAL_FOURCC('Y', '7', '0', '9')
#define MMAL_COLOR_SPACE_JPEG_JFIF MMAL_FOURCC('Y', 'J', 'F', 'I')
#define MMAL_COLOR_SPACE_FCC MMAL_FOURCC('Y', 'F', 'C', 'C')
#define MMAL_COLOR_SPACE_SMPTE240M MMAL_FOURCC('Y', '2', '4', '0')
#define MMAL_COLOR_SPACE_BT470_2_M MMAL_FOURCC('Y', '_', '_', 'M')
#define MMAL_COLOR_SPACE_BT470_2_BG MMAL_FOURCC('Y', '_', 'B', 'G')
#define MMAL_COLOR_SPACE_JFIF_Y16_255 MMAL_FOURCC('Y', 'Y', '1', '6')

/* Buffer flags (mmal_buffer.flags) */
#define MMAL_BUFFER_HEADER_FLAG_EOS (1 << 0)
#define MMAL_BUFFER_HEADER_FLAG_FRAME_START (1 << 1)
#define MMAL_BUFFER_HEADER_FLAG_FRAME_END (1 << 2)
#define MMAL_BUFFER_HEADER_FLAG_FRAME (MMAL_BUFFER_HEADER_FLAG_FRAME_START | MMAL_BUFFER_HEADER_FLAG_FRAME_END)
#define MMAL_BUFFER_HEADER_FLAG_KEYFRAME (1 << 3)
#define MMAL_BUFFER_HEADER_FLAG_DISCONTINUITY (1 << 4)
#define MMAL_BUFFER_HEADER_FLAG_CONFIG (1 << 5)
#define MMAL_BUFFER_HEADER_FLAG_ENCRYPTED (1 << 6)
#define MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO (1 << 7)
#define MMAL_BUFFER_HEADER_FLAGS_SNAPSHOT (1 << 8)
#define MMAL_BUFFER_HEADER_FLAG_CORRUPTED (1 << 9)
#define MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED (1 << 10)
#define MMAL_BUFFER_HEADER_FLAG_DECODEONLY (1 << 11)
#define MMAL_BUFFER_HEADER_FLAG_NAL_END (1 << 12)
#define MMAL_BUFFER_HEADER_FLAG_USER0 (1 << 28)
#define MMAL_BUFFER_HEADER_FLAG_USER1 (1 << 29)
#define MMAL_BUFFER_HEADER_FLAG_USER2 (1 << 30)
#define MMAL_BUFFER_HEADER_FLAG_USER3 (1UL << 31)
#define MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED (1 << 16)
#define MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST (1 << 17)
#define MMAL_BUFFER_HEADER_VIDEO_FLAG_DISPLAY_EXTERNAL (1 << 19)
#define MMAL_BUFFER_HEADER_VIDEO_FLAG_PROTECTED (1 << 20)

/* Events (mmal_buffer.cmd of an event) */
#define MMAL_EVENT_ERROR MMAL_FOURCC('E', 'R', 'R', 'O')             /* data: one ULONG, the MMAL status */
#define MMAL_EVENT_EOS MMAL_FOURCC('E', 'E', 'O', 'S')               /* data: port type and index */
#define MMAL_EVENT_FORMAT_CHANGED MMAL_FOURCC('E', 'F', 'C', 'H')    /* data: struct mmal_format_change */
#define MMAL_EVENT_PARAMETER_CHANGED MMAL_FOURCC('E', 'P', 'C', 'H') /* data: the parameter, words native */

/*
 * Parameter ids.  The bodies are those of Broadcom's mmal_parameters*.h:
 * 32-bit words, which mmal_port_set() and mmal_port_get() convert.  The few
 * bodies of bytes or 64-bit values (URI, SEEK, SYSTEM_TIME, EXIF) are not
 * used.
 */
#define MMAL_PARAMETER_GROUP_COMMON (0 << 16)
#define MMAL_PARAMETER_GROUP_CAMERA (1 << 16)
#define MMAL_PARAMETER_GROUP_VIDEO (2 << 16)
#define MMAL_PARAMETER_GROUP_AUDIO (3 << 16)
#define MMAL_PARAMETER_GROUP_CLOCK (4 << 16)
#define MMAL_PARAMETER_GROUP_MIRACAST (5 << 16)

enum
{
	MMAL_PARAMETER_UNUSED = MMAL_PARAMETER_GROUP_COMMON,
	MMAL_PARAMETER_SUPPORTED_ENCODINGS,  /* read: the encodings a port takes, a fourcc a word */
	MMAL_PARAMETER_URI,
	MMAL_PARAMETER_CHANGE_EVENT_REQUEST, /* {change_id, enable} */
	MMAL_PARAMETER_ZERO_COPY,
	MMAL_PARAMETER_BUFFER_REQUIREMENTS,
	MMAL_PARAMETER_STATISTICS,
	MMAL_PARAMETER_CORE_STATISTICS,
	MMAL_PARAMETER_MEM_USAGE,
	MMAL_PARAMETER_BUFFER_FLAG_FILTER,
	MMAL_PARAMETER_SEEK,
	MMAL_PARAMETER_POWERMON_ENABLE,
	MMAL_PARAMETER_LOGGING,
	MMAL_PARAMETER_SYSTEM_TIME,
	MMAL_PARAMETER_NO_IMAGE_PADDING, /* 1: planes exactly as the format's width and height lay them out, no alignment padding */
	MMAL_PARAMETER_LOCKSTEP_ENABLE,
};

enum
{
	MMAL_PARAMETER_DISPLAYREGION = MMAL_PARAMETER_GROUP_VIDEO,
	MMAL_PARAMETER_SUPPORTED_PROFILES,
	MMAL_PARAMETER_PROFILE,     /* encoder: {MMAL_VIDEO_PROFILE_*, MMAL_VIDEO_LEVEL_*} */
	MMAL_PARAMETER_INTRAPERIOD, /* encoder: frames from one key frame to the next */
	MMAL_PARAMETER_RATECONTROL, /* encoder: MMAL_VIDEO_RATECONTROL_*, variable or constant bit rate */
	MMAL_PARAMETER_NALUNITFORMAT,
	MMAL_PARAMETER_MINIMISE_FRAGMENTATION, /* encoder: 1 keeps a frame in one buffer, split only when it does not fit */
	MMAL_PARAMETER_MB_ROWS_PER_SLICE,
	MMAL_PARAMETER_VIDEO_LEVEL_EXTENSION,
	MMAL_PARAMETER_VIDEO_EEDE_ENABLE,
	MMAL_PARAMETER_VIDEO_EEDE_LOSSRATE,
	MMAL_PARAMETER_VIDEO_REQUEST_I_FRAME, /* encoder: 1 makes the next frame a key frame (while running) */
	MMAL_PARAMETER_VIDEO_INTRA_REFRESH,
	MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, /* encoder: 1 leaves the input picture as it was; 0 lets it write its reconstruction over it */
	MMAL_PARAMETER_VIDEO_BIT_RATE,        /* encoder: the target, bits per second (also while running) */
	MMAL_PARAMETER_VIDEO_FRAME_RATE,
	MMAL_PARAMETER_VIDEO_ENCODE_MIN_QUANT,
	MMAL_PARAMETER_VIDEO_ENCODE_MAX_QUANT,
	MMAL_PARAMETER_VIDEO_ENCODE_RC_MODEL,
	MMAL_PARAMETER_EXTRA_BUFFERS,
	MMAL_PARAMETER_VIDEO_ALIGN_HORIZ,
	MMAL_PARAMETER_VIDEO_ALIGN_VERT,
	MMAL_PARAMETER_VIDEO_DROPPABLE_PFRAMES,
	MMAL_PARAMETER_VIDEO_ENCODE_INITIAL_QUANT,
	MMAL_PARAMETER_VIDEO_ENCODE_QP_P,
	MMAL_PARAMETER_VIDEO_ENCODE_RC_SLICE_DQUANT,
	MMAL_PARAMETER_VIDEO_ENCODE_FRAME_LIMIT_BITS,
	MMAL_PARAMETER_VIDEO_ENCODE_PEAK_RATE,
	MMAL_PARAMETER_VIDEO_ENCODE_H264_DISABLE_CABAC,
	MMAL_PARAMETER_VIDEO_ENCODE_H264_LOW_LATENCY,
	MMAL_PARAMETER_VIDEO_ENCODE_H264_AU_DELIMITERS,
	MMAL_PARAMETER_VIDEO_ENCODE_H264_DEBLOCK_IDC,
	MMAL_PARAMETER_VIDEO_ENCODE_H264_MB_INTRA_MODE,
	MMAL_PARAMETER_VIDEO_ENCODE_HEADER_ON_OPEN, /* encoder: 1 sends SPS/PPS in a buffer of their own when the port is enabled */
	MMAL_PARAMETER_VIDEO_ENCODE_PRECODE_FOR_QP,
	MMAL_PARAMETER_VIDEO_DRM_INIT_INFO,
	MMAL_PARAMETER_VIDEO_TIMESTAMP_FIFO,
	MMAL_PARAMETER_VIDEO_DECODE_ERROR_CONCEALMENT,
	MMAL_PARAMETER_VIDEO_DRM_PROTECT_BUFFER,
	MMAL_PARAMETER_VIDEO_DECODE_CONFIG_VD3,
	MMAL_PARAMETER_VIDEO_ENCODE_H264_VCL_HRD_PARAMETERS,
	MMAL_PARAMETER_VIDEO_ENCODE_H264_LOW_DELAY_HRD_FLAG,
	MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, /* encoder: 1 repeats SPS/PPS before every key frame */
	MMAL_PARAMETER_VIDEO_ENCODE_SEI_ENABLE,
	MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS,
	MMAL_PARAMETER_VIDEO_RENDER_STATS,
	MMAL_PARAMETER_VIDEO_INTERLACE_TYPE, /* {MMAL_INTERLACE_*, repeat first field} */
	MMAL_PARAMETER_VIDEO_INTERPOLATE_TIMESTAMPS, /* decoder: 1 makes up a PTS for a frame that came without one; 0 passes on what came */
	MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING,      /* encoder: 1 writes the frame rate into the SPS (VUI timing), from the output format */
	MMAL_PARAMETER_VIDEO_MAX_NUM_CALLBACKS,
	MMAL_PARAMETER_VIDEO_SOURCE_PATTERN,
	MMAL_PARAMETER_VIDEO_ENCODE_SEPARATE_NAL_BUFS,
	MMAL_PARAMETER_VIDEO_DROPPABLE_PFRAME_LENGTH,
	MMAL_PARAMETER_VIDEO_STALL_THRESHOLD,
	MMAL_PARAMETER_VIDEO_ENCODE_HEADERS_WITH_FRAME, /* encoder: 1 puts SPS/PPS in the first frame's buffer, not one of their own */
	MMAL_PARAMETER_VIDEO_VALIDATE_TIMESTAMPS,
	MMAL_PARAMETER_VIDEO_STOP_ON_PAR_COLOUR_CHANGE,
};

/* The camera group holds the JPEG, crop and mirror parameters of the image components too (mmal_parameters_camera.h). */
#define MMAL_PARAMETER_THUMBNAIL_CONFIGURATION (MMAL_PARAMETER_GROUP_CAMERA + 0x00) /* {enable, width, height, quality} on control */
#define MMAL_PARAMETER_EXIF_DISABLE (MMAL_PARAMETER_GROUP_CAMERA + 0x03)            /* 1: a JPEG without an EXIF block */
#define MMAL_PARAMETER_EXIF (MMAL_PARAMETER_GROUP_CAMERA + 0x04)                    /* "key=value" bytes */
#define MMAL_PARAMETER_MIRROR (MMAL_PARAMETER_GROUP_CAMERA + 0x0f)                  /* MMAL_PARAM_MIRROR_* */
#define MMAL_PARAMETER_JPEG_Q_FACTOR (MMAL_PARAMETER_GROUP_CAMERA + 0x19)           /* the JPEG quality, 1..100, on the output */
#define MMAL_PARAMETER_JPEG_RESTART_INTERVAL (MMAL_PARAMETER_GROUP_CAMERA + 0x50)
#define MMAL_PARAMETER_CROP (MMAL_PARAMETER_GROUP_CAMERA + 0x55)                    /* {x, y, width, height} of the input picture used */
#define MMAL_PARAMETER_JPEG_IJG_SCALING (MMAL_PARAMETER_GROUP_CAMERA + 0x61)        /* 0 or 1 */

#define MMAL_PARAM_MIRROR_NONE 0
#define MMAL_PARAM_MIRROR_VERTICAL 1
#define MMAL_PARAM_MIRROR_HORIZONTAL 2
#define MMAL_PARAM_MIRROR_BOTH 3

/* MMAL_PARAMETER_RATECONTROL */
#define MMAL_VIDEO_RATECONTROL_DEFAULT 0
#define MMAL_VIDEO_RATECONTROL_VARIABLE 1
#define MMAL_VIDEO_RATECONTROL_CONSTANT 2

/* H.264 profiles and levels (MMAL_PARAMETER_PROFILE) */
#define MMAL_VIDEO_PROFILE_H264_BASELINE 25
#define MMAL_VIDEO_PROFILE_H264_MAIN 26
#define MMAL_VIDEO_PROFILE_H264_EXTENDED 27
#define MMAL_VIDEO_PROFILE_H264_HIGH 28
#define MMAL_VIDEO_PROFILE_H264_HIGH10 29
#define MMAL_VIDEO_PROFILE_H264_HIGH422 30
#define MMAL_VIDEO_PROFILE_H264_HIGH444 31
#define MMAL_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE 32
#define MMAL_VIDEO_LEVEL_H264_1 17
#define MMAL_VIDEO_LEVEL_H264_1b 18
#define MMAL_VIDEO_LEVEL_H264_11 19
#define MMAL_VIDEO_LEVEL_H264_12 20
#define MMAL_VIDEO_LEVEL_H264_13 21
#define MMAL_VIDEO_LEVEL_H264_2 22
#define MMAL_VIDEO_LEVEL_H264_21 23
#define MMAL_VIDEO_LEVEL_H264_22 24
#define MMAL_VIDEO_LEVEL_H264_3 25
#define MMAL_VIDEO_LEVEL_H264_31 26
#define MMAL_VIDEO_LEVEL_H264_32 27
#define MMAL_VIDEO_LEVEL_H264_4 28
#define MMAL_VIDEO_LEVEL_H264_41 29
#define MMAL_VIDEO_LEVEL_H264_42 30
#define MMAL_VIDEO_LEVEL_H264_5 31
#define MMAL_VIDEO_LEVEL_H264_51 32

/* MMAL_PARAMETER_VIDEO_INTERLACE_TYPE modes */
#define MMAL_INTERLACE_PROGRESSIVE 0
#define MMAL_INTERLACE_FIELD_SINGLE_UPPER_FIRST 1
#define MMAL_INTERLACE_FIELD_SINGLE_LOWER_FIRST 2
#define MMAL_INTERLACE_FIELDS_INTERLEAVED_UPPER_FIRST 3
#define MMAL_INTERLACE_FIELDS_INTERLEAVED_LOWER_FIRST 4
#define MMAL_INTERLACE_MIXED 5

#define MMAL_EXTRADATA_MAX 128 /* codec configuration bytes a format can carry */

/* A rectangle: the visible part of a picture, or a crop */
struct mmal_rect
{
	s32 x;
	s32 y;
	s32 width;
	s32 height;
};

struct mmal_rational
{
	s32 num;
	s32 den;
};

/* An elementary stream format (MMAL_ES_FORMAT_T with its type-specific part and extradata inline) */
struct mmal_format
{
	u32 type; /* MMAL_ES_TYPE_* */
	u32 encoding;
	u32 encoding_variant;
	u32 bitrate;
	u32 flags; /* MMAL_ES_FORMAT_FLAG_* */
	union
	{
		struct
		{
			u32 width;  /* of the frame in memory: the VPU aligns it (32 for I420) */
			u32 height; /* likewise (16) */
			struct mmal_rect crop; /* the visible picture */
			struct mmal_rational frame_rate;
			struct mmal_rational par; /* pixel aspect ratio */
			u32 color_space;
		} video;
		struct
		{
			u32 channels;
			u32 sample_rate;
			u32 bits_per_sample;
			u32 block_align;
		} audio;
		struct
		{
			u32 x_offset;
			u32 y_offset;
		} subpicture;
		u32 words[11];
	} es;
	u32 extradata_size;
	u8 extradata[MMAL_EXTRADATA_MAX];
};

/* The FORMAT_CHANGED event's data, decoded (MMAL_EVENT_FORMAT_CHANGED_T, the format inline): a port's new format and buffer needs */
struct mmal_format_change
{
	u32 buffer_size_min;
	u32 buffer_num_min;
	u32 buffer_size_recommended;
	u32 buffer_num_recommended;
	struct mmal_format format; /* copy it into the port's format and commit after a disable */
};

#endif /* VIDEOCORE_MMAL_H */
