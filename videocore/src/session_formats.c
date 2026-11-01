/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * session_formats.c - pixel formats, picture layout, video formats,
 * rectangles, and the translations between MMAL and the client: packet
 * flags, frame and packet descriptions.  Nothing here talks to the VPU.
 */
#include <memory.h>

#include <videocore_private.h>
#include <session_internal.h>

/* ---- pixel formats ---------------------------------------------------------------- */

/**
 * format_is_planar() - Whether a format keeps luma and chroma in planes of their own.
 * @format: VCF_*
 *
 * Return: TRUE for I420, YV12 and NV12.
 */
BOOL format_is_planar(ULONG format)
{
	return format == VCF_I420 || format == VCF_YV12 || format == VCF_NV12;
}

/**
 * format_bytes_per_pixel() - Bytes per pixel of a format's first plane.
 * @format: VCF_*
 *
 * Return: the bytes of a packed format, 1 for a planar one (its luma), 0 for
 * a format this backend does not know.
 */
u32 format_bytes_per_pixel(ULONG format)
{
	switch (format)
	{
	case VCF_I420:
	case VCF_YV12:
	case VCF_NV12:
		return 1;
	case VCF_RGB16:
	case VCF_YUYV:
	case VCF_UYVY:
		return 2;
	case VCF_RGB24:
	case VCF_BGR24:
		return 3;
	case VCF_BGRA32:
	case VCF_RGBA32:
		return 4;
	default:
		return 0;
	}
}

/**
 * color_space_mmal() - The MMAL colour space of a VCCS_*.
 * @space: VCCS_*
 *
 * Return: MMAL_COLOR_SPACE_*, unknown for anything else.
 */
u32 color_space_mmal(ULONG space)
{
	switch (space)
	{
	case VCCS_BT601:
		return MMAL_COLOR_SPACE_ITUR_BT601;
	case VCCS_BT709:
		return MMAL_COLOR_SPACE_ITUR_BT709;
	case VCCS_JPEG:
		return MMAL_COLOR_SPACE_JPEG_JFIF;
	default:
		return MMAL_COLOR_SPACE_UNKNOWN;
	}
}

/**
 * format_encoding() - The MMAL encoding of a pixel format.
 * @format: VCF_*
 *
 * Return: MMAL_ENCODING_*, 0 for none.
 */
ULONG format_encoding(ULONG format)
{
	switch (format)
	{
	case VCF_I420:
		return MMAL_ENCODING_I420;
	case VCF_YV12:
		return MMAL_ENCODING_YV12;
	case VCF_NV12:
		return MMAL_ENCODING_NV12;
	case VCF_RGB16:
		return MMAL_ENCODING_RGB16;
	case VCF_RGB24:
		return MMAL_ENCODING_RGB24;
	case VCF_BGR24:
		return MMAL_ENCODING_BGR24;
	case VCF_BGRA32:
		return MMAL_ENCODING_BGRA;
	case VCF_RGBA32:
		return MMAL_ENCODING_RGBA;
	case VCF_YUYV:
		return MMAL_ENCODING_YUYV;
	case VCF_UYVY:
		return MMAL_ENCODING_UYVY;
	default:
		return 0;
	}
}

/**
 * video_codec_encoding() - The encoding ril.video_decode's input takes for a video codec.
 * @codec: VCC_*
 *
 * Its input takes H264, MVC and MJPG only (VCInfo COMPONENTS): streams.  A
 * still picture goes to ril.image_decode (image_codec_encoding()).
 *
 * Return: MMAL_ENCODING_*, 0 for one ril.video_decode does not decode.
 */
ULONG video_codec_encoding(ULONG codec)
{
	switch (codec)
	{
	case VCC_H264:
		return MMAL_ENCODING_H264;
	case VCC_MJPEG:
		return MMAL_ENCODING_MJPEG;
	default:
		return 0;
	}
}

/**
 * image_codec_encoding() - The encoding ril.image_decode's input takes for a still picture's codec.
 * @codec: VCC_*
 *
 * Return: MMAL_ENCODING_*, 0 for a codec that is no still picture.
 */
ULONG image_codec_encoding(ULONG codec)
{
	static const struct
	{
		ULONG codec, encoding;
	} stills[] = {
	    {VCC_JPEG, MMAL_ENCODING_JPEG}, {VCC_PNG, MMAL_ENCODING_PNG}, {VCC_GIF, MMAL_ENCODING_GIF},
	    {VCC_BMP, MMAL_ENCODING_BMP},   {VCC_TGA, MMAL_ENCODING_TGA}, {VCC_PPM, MMAL_ENCODING_PPM},
	};
	for (u32 i = 0; i < ARRAY_SIZE(stills); i++)
		if (stills[i].codec == codec)
			return stills[i].encoding;
	return 0;
}

/**
 * picture_geometry() - A picture's layout in its buffer.
 * @format:   VCF_*
 * @width:    pixels per row of the buffer (not of the visible picture)
 * @height:   rows
 * @pitch:    bytes per row, 0 for the natural one
 * @geometry: where the layout goes
 *
 * Planar pictures take the VPU's alignment: rows of a multiple of 32 pixels,
 * a multiple of 16 of them.  A packed picture's pitch is whole pixels: an
 * MMAL format has no stride, the VPU works it out from the width.
 *
 * Return: TRUE, FALSE for an unknown format, an empty picture, or a pitch too
 * small for the width or not of whole pixels.
 */
BOOL picture_geometry(ULONG format, u32 width, u32 height, u32 pitch, struct vc_geometry *geometry)
{
	u32 bytes_per_pixel = format_bytes_per_pixel(format);
	if (bytes_per_pixel == 0 || width == 0 || height == 0)
		return FALSE;
	memset(geometry, 0, sizeof(*geometry));
	if (format_is_planar(format))
	{
		u32 rows = ALIGN_UP(height, 16u);
		u32 row_bytes = pitch != 0 ? pitch : ALIGN_UP(width, 32u);
		if (row_bytes < width || (row_bytes & 1u) != 0)
			return FALSE;
		u32 luma = row_bytes * rows;
		geometry->width = row_bytes;
		geometry->height = rows;
		geometry->pitch[0] = row_bytes;
		if (format == VCF_NV12)
		{
			geometry->planes = 2;
			geometry->pitch[1] = row_bytes;
			geometry->offset[1] = luma;
			geometry->size = luma + row_bytes * (rows / 2u);
		}
		else
		{
			u32 chroma = (row_bytes / 2u) * (rows / 2u);
			geometry->planes = 3;
			geometry->pitch[1] = geometry->pitch[2] = row_bytes / 2u;
			geometry->offset[1] = format == VCF_I420 ? luma : luma + chroma; /* YV12 has V first */
			geometry->offset[2] = format == VCF_I420 ? luma + chroma : luma;
			geometry->size = luma + 2u * chroma;
		}
		return TRUE;
	}
	u32 row_bytes = pitch != 0 ? pitch : width * bytes_per_pixel;
	if (row_bytes < width * bytes_per_pixel || row_bytes % bytes_per_pixel != 0)
		return FALSE;
	geometry->planes = 1;
	geometry->width = row_bytes / bytes_per_pixel;
	geometry->height = height;
	geometry->pitch[0] = row_bytes;
	geometry->size = row_bytes * height;
	return TRUE;
}

/**
 * format_crop_set() - The visible picture of a video format.
 * @format: the format
 * @crop:   the rectangle
 */
static void format_crop_set(struct mmal_format *format, const struct VCRect *crop)
{
	format->es.video.crop.x = crop->X;
	format->es.video.crop.y = crop->Y;
	format->es.video.crop.width = crop->Width;
	format->es.video.crop.height = crop->Height;
}

/**
 * video_format_set() - A video format: encoding, the picture's buffer, the visible picture, the rate.
 * @format:   the format, cleared first
 * @encoding: MMAL_ENCODING_*
 * @width:    the buffer's pixels per row
 * @height:   its rows
 * @crop:     the visible picture
 * @rate:     frames per second; a 0 denominator gives an unknown rate (0/1)
 *
 * Square pixels, no colour space, no codec configuration.
 */
void video_format_set(struct mmal_format *format, u32 encoding, u32 width, u32 height, const struct VCRect *crop,
                      const struct VCRational *rate)
{
	memset(format, 0, sizeof(*format));
	format->type = MMAL_ES_TYPE_VIDEO;
	format->encoding = encoding;
	format->es.video.width = width;
	format->es.video.height = height;
	format_crop_set(format, crop);
	format->es.video.frame_rate.num = rate->Den != 0 ? rate->Num : 0;
	format->es.video.frame_rate.den = rate->Den != 0 ? rate->Den : 1;
	format->es.video.par.num = 1;
	format->es.video.par.den = 1;
}

/* ---- rectangles ------------------------------------------------------------------ */

/**
 * rect_whole() - The whole of a picture as a rectangle.
 * @width:  its width
 * @height: its height
 *
 * Return: the rectangle at 0,0.
 */
struct VCRect rect_whole(u32 width, u32 height)
{
	struct VCRect rect = {0, 0, (LONG)width, (LONG)height};
	return rect;
}

/**
 * rect_inside() - Whether a rectangle is a non-empty part of a picture.
 * @rect:   the rectangle
 * @width:  the picture's width
 * @height: its height
 *
 * Return: TRUE if it is not empty and lies within the picture.
 */
BOOL rect_inside(const struct VCRect *rect, u32 width, u32 height)
{
	return rect->X >= 0 && rect->Y >= 0 && rect->Width > 0 && rect->Height > 0 && (u32)(rect->X + rect->Width) <= width &&
	       (u32)(rect->Y + rect->Height) <= height;
}

/**
 * rect_equal() - Whether two rectangles are the same.
 * @first:  one
 * @second: the other
 *
 * Return: TRUE if place and size match.
 */
BOOL rect_equal(const struct VCRect *first, const struct VCRect *second)
{
	return first->X == second->X && first->Y == second->Y && first->Width == second->Width && first->Height == second->Height;
}

/* ---- frames ---------------------------------------------------------------------- */

/**
 * frame_set_layout() - Describe a frame's buffer to the client: size, planes, offsets, pitches.
 * @frame:    the client's frame
 * @geometry: the layout
 */
static void frame_set_layout(struct VCFrame *frame, const struct vc_geometry *geometry)
{
	frame->Width = geometry->width;
	frame->Height = geometry->height;
	frame->Planes = geometry->planes;
	for (u32 i = 0; i < 3; i++)
	{
		frame->Offset[i] = geometry->offset[i];
		frame->Pitch[i] = geometry->pitch[i];
	}
}

/**
 * frame_set_geometry() - Describe a frame object to the client: format, layout, visible picture.
 * @object:   a frame object
 * @geometry: its layout
 * @format:   VCF_*
 * @crop:     the visible picture inside it
 */
void frame_set_geometry(struct vc_object *object, const struct vc_geometry *geometry, ULONG format, const struct VCRect *crop)
{
	struct VCFrame *frame = &object->pub.frame;
	frame->Format = format;
	frame->Crop = *crop;
	frame_set_layout(frame, geometry);
}

/**
 * frame_from_native() - Describe a decoded native frame as the VPU laid it out.
 * @object:   the frame object, back from the VPU
 * @geometry: the decoder output's layout: I420 for video, a still's format
 *
 * An I420 frame: the planes the buffer names when it names them, @geometry
 * otherwise.  A packed one: @geometry (a converted still's header still
 * names the planes of the picture it was converted from).
 */
void frame_from_native(struct vc_object *object, const struct vc_geometry *geometry)
{
	const struct mmal_buffer *buffer = &object->buffer;
	struct VCFrame *frame = &object->pub.frame;
	/*
	 * The VPU's layout only if it described one that holds the picture: I420's
	 * three planes (0: not described), a luma pitch at least the visible
	 * width, the chroma after the luma (offset 0: not filled in).  The pitch
	 * is then the width (luma: a byte a pixel); the header has no height.
	 */
	if (geometry->planes >= 3 && buffer->video.planes >= 3 && buffer->video.pitch[0] >= (ULONG)frame->Crop.Width &&
	    buffer->video.offset[1] != 0)
	{
		frame->Planes = 3;
		for (u32 i = 0; i < 3; i++)
		{
			frame->Offset[i] = buffer->video.offset[i];
			frame->Pitch[i] = buffer->video.pitch[i];
		}
		frame->Width = buffer->video.pitch[0];
		frame->Height = geometry->height;
	}
	else
		frame_set_layout(frame, geometry);
}

/**
 * frame_from_return() - A returned frame's time and flags, for the client.
 * @object: the frame object, back from the VPU
 */
void frame_from_return(struct vc_object *object)
{
	const struct mmal_buffer *buffer = &object->buffer;
	struct VCFrame *frame = &object->pub.frame;
	frame->Pts = buffer->pts;
	frame->Flags = 0;
	if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME)
		frame->Flags |= VCFF_KeyFrame;
	if (buffer->video.flags & MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED)
		frame->Flags |= VCFF_Interlaced;
	if (buffer->video.flags & MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST)
		frame->Flags |= VCFF_TopFieldFirst;
}

/* ---- packets --------------------------------------------------------------------- */

/* The packet flags and their MMAL buffer flags, one to one */
static const struct
{
	ULONG packet, mmal;
} packet_flags[] = {
    {VCPF_KeyFrame, MMAL_BUFFER_HEADER_FLAG_KEYFRAME},      {VCPF_Config, MMAL_BUFFER_HEADER_FLAG_CONFIG},
    {VCPF_FrameStart, MMAL_BUFFER_HEADER_FLAG_FRAME_START}, {VCPF_FrameEnd, MMAL_BUFFER_HEADER_FLAG_FRAME_END},
    {VCPF_EndOfStream, MMAL_BUFFER_HEADER_FLAG_EOS},        {VCPF_Corrupt, MMAL_BUFFER_HEADER_FLAG_CORRUPTED},
};

/**
 * packet_flags_to_mmal() - A packet's VCPF_* flags as MMAL buffer flags.
 * @flags: VCPF_*
 *
 * Return: MMAL_BUFFER_HEADER_FLAG_*.
 */
ULONG packet_flags_to_mmal(ULONG flags)
{
	ULONG mmal = 0;
	for (u32 i = 0; i < ARRAY_SIZE(packet_flags); i++)
		if (flags & packet_flags[i].packet)
			mmal |= packet_flags[i].mmal;
	return mmal;
}

/**
 * packet_from_return() - An encoded packet as the VPU returned it, for the client.
 * @object: the packet object, back from the VPU
 */
void packet_from_return(struct vc_object *object)
{
	const struct mmal_buffer *buffer = &object->buffer;
	struct VCPacket *packet = &object->pub.packet;
	packet->Length = buffer->length;
	packet->Pts = buffer->pts;
	ULONG flags = 0;
	for (u32 i = 0; i < ARRAY_SIZE(packet_flags); i++)
		if (buffer->flags & packet_flags[i].mmal)
			flags |= packet_flags[i].packet;
	packet->Flags = flags;
}
