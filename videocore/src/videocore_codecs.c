/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_codecs.c - what this backend does: the codec table the frontend
 * reads (VCB_QueryCodec()), and the questions asked of it.
 */
#include <memory.h>
#include <strutil.h>

#include <videocore_private.h>
#include <session_internal.h>

/* What the ISP writes, and what the H.264 encoder reads: it goes through the ISP for all but I420 */
#define ISP_FORMATS \
	(VCF_MASK(VCF_I420) | VCF_MASK(VCF_RGB16) | VCF_MASK(VCF_RGB24) | VCF_MASK(VCF_BGR24) | VCF_MASK(VCF_BGRA32) | VCF_MASK(VCF_RGBA32))
/* What the converter reads: the ISP's own formats and a camera's YUYV and NV12 */
#define ISP_INPUT_FORMATS (ISP_FORMATS | VCF_MASK(VCF_YUYV) | VCF_MASK(VCF_NV12))
#define JPEG_INPUT_FORMATS (VCF_MASK(VCF_I420) | VCF_MASK(VCF_RGBA32))

#define BOTH (VCCF_Session | VCCF_Image)

/*
 * The still pictures are image calls only (videocore_image.c), written in
 * any format: by the pipeline itself where it writes that format and
 * layout, else through the converter.  output_formats is what a decoder or
 * converter writes; input_formats what an encoder or converter reads, which
 * its sessions keep to (codec_reads()).
 */
static const struct
{
	UWORD codec, kind, max_level, flags;
	ULONG output_formats, input_formats;
} codec_table[] = {
    {VCC_H264, VCK_Decoder, 42, VCCF_Session, ISP_FORMATS, 0}, /* in real time at 4.2's ceiling; the level is not checked */
    {VCC_MJPEG, VCK_Decoder, 0, VCCF_Session, ISP_FORMATS, 0},
    {VCC_JPEG, VCK_Decoder, 0, VCCF_Image, ISP_FORMATS, 0},
    {VCC_PNG, VCK_Decoder, 0, VCCF_Image, ISP_FORMATS, 0},
    {VCC_GIF, VCK_Decoder, 0, VCCF_Image, ISP_FORMATS, 0},
    {VCC_BMP, VCK_Decoder, 0, VCCF_Image, ISP_FORMATS, 0},
    {VCC_TGA, VCK_Decoder, 0, VCCF_Image, ISP_FORMATS, 0},
    {VCC_PPM, VCK_Decoder, 0, VCCF_Image, ISP_FORMATS, 0},
    {VCC_Raw, VCK_Converter, 0, BOTH, ISP_FORMATS, ISP_INPUT_FORMATS},
    {VCC_JPEG, VCK_Encoder, 0, BOTH, 0, JPEG_INPUT_FORMATS},
    {VCC_H264, VCK_Encoder, 40, VCCF_Session, 0, ISP_FORMATS}, /* specified at level 4.0; higher levels in the headers */
};

/**
 * codec_still() - Whether a codec is a still picture's, which only the image calls decode.
 * @codec: VCC_*
 *
 * Return: TRUE for JPEG, PNG, GIF, BMP, TGA and PPM.
 */
BOOL codec_still(ULONG codec)
{
	return image_codec_encoding(codec) != 0;
}

/**
 * codec_image_allowed() - Whether the image calls take a kind and codec.
 * @kind:  VCK_*
 * @codec: VCC_*
 *
 * Return: TRUE for a row of the table with VCCF_Image.
 */
BOOL codec_image_allowed(ULONG kind, ULONG codec)
{
	for (u32 i = 0; i < ARRAY_SIZE(codec_table); i++)
		if (codec_table[i].kind == kind && codec_table[i].codec == codec)
			return (codec_table[i].flags & VCCF_Image) != 0;
	return FALSE;
}

/**
 * codec_reads() - Whether an encoder or the converter reads a pixel format.
 * @kind:   VCK_*
 * @codec:  VCC_*
 * @format: VCF_*
 *
 * Return: TRUE for a format of the row's input formats.
 */
BOOL codec_reads(ULONG kind, ULONG codec, ULONG format)
{
	for (u32 i = 0; i < ARRAY_SIZE(codec_table); i++)
		if (codec_table[i].kind == kind && codec_table[i].codec == codec)
			return format < 32u && (codec_table[i].input_formats & VCF_MASK(format)) != 0;
	return FALSE;
}

/**
 * codec_count() - The rows of the codec table.
 *
 * Return: how many rows codec_query() answers.
 */
ULONG codec_count(void)
{
	return ARRAY_SIZE(codec_table);
}

/**
 * codec_query() - One row of the codec table (VCB_QueryCodec()).
 * @index: the row, 0 upward
 * @info:  the caller's structure, filled as far as its Size reaches
 *
 * Every codec here runs on the VPU and claims 1920x1080.  The H.264 decoder
 * takes other shapes up to 2032 pixels wide and 8192 macroblocks too
 * (1080x1920 among them), and refuses larger ones (session_decoder.c).
 *
 * Return: 0, or VCERR_BADPARAM past the last row.
 */
LONG codec_query(ULONG index, struct VCCodecInfo *info)
{
	if (index >= codec_count())
		return VCERR_BADPARAM;
	struct VCCodecInfo row;
	memset(&row, 0, sizeof(row));
	row.Size = sizeof(row);
	row.Codec = codec_table[index].codec;
	row.Kind = codec_table[index].kind;
	row.MaxWidth = 1920;
	row.MaxHeight = 1080;
	row.MaxLevel = codec_table[index].max_level;
	row.OutputFormats = codec_table[index].output_formats;
	strlcpy((char *)row.Backend, LIBRARY_NAME, sizeof(row.Backend));
	row.Priority = VCB_PRIORITY_HARDWARE;
	row.Flags = codec_table[index].flags;
	row.InputFormats = codec_table[index].input_formats;
	ULONG size = umin(info->Size, sizeof(row));
	memcpy(info, &row, size);
	info->Size = size;
	return 0;
}
