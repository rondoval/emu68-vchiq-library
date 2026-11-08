/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vc_names.c - videocodec.library's codecs, kinds and pixel formats by name.
 */
#include <libraries/videocodec.h>
#include <strutil.h>

#include "vc_names.h"

/* VCF_* by number */
static const char *const format_names[] = {
    "?",      "I420",   "YV12",   "NV12",    "RGB16", "RGB24",   "BGR24", "BGRA32", "RGBA32",
    "YUYV",   "UYVY",   "NV12_COL128", "NV21", "I422", "YVYU", "VYUY", "RGBX32", "BGRX32",
    "ARGB32", "ABGR32", "RGB16BE", "RGB15", "RGB15BE", "CLUT8", "NV12_10_COL128",
};

/**
 * codec_name() - A codec's name.
 * @codec: VCC_*
 *
 * Return: the name, "?" for one this does not know.
 */
const char *codec_name(ULONG codec)
{
	switch (codec)
	{
	case VCC_H264:
		return "H264";
	case VCC_MJPEG:
		return "MJPEG";
	case VCC_JPEG:
		return "JPEG";
	case VCC_H265:
		return "H265";
	case VCC_Raw:
		return "raw";
	case VCC_PNG:
		return "PNG";
	case VCC_GIF:
		return "GIF";
	case VCC_BMP:
		return "BMP";
	case VCC_TGA:
		return "TGA";
	case VCC_PPM:
		return "PPM";
	case VCC_MPEG1:
		return "MPEG1";
	case VCC_MPEG2:
		return "MPEG2";
	case VCC_MPEG4:
		return "MPEG4";
	case VCC_MSMPEG4:
		return "MSMPEG4";
	case VCC_H263:
		return "H263";
	case VCC_VP8:
		return "VP8";
	case VCC_VP9:
		return "VP9";
	case VCC_AV1:
		return "AV1";
	case VCC_VC1:
		return "VC1";
	case VCC_Theora:
		return "Theora";
	case VCC_Cinepak:
		return "Cinepak";
	case VCC_MSVideo1:
		return "MSVideo1";
	case VCC_QTRLE:
		return "QTRLE";
	default:
		return "?";
	}
}

/**
 * kind_name() - A codec kind's name.
 * @kind: VCK_*
 *
 * Return: the name, "?" for one this does not know.
 */
const char *kind_name(ULONG kind)
{
	switch (kind)
	{
	case VCK_Decoder:
		return "decoder";
	case VCK_Encoder:
		return "encoder";
	case VCK_Converter:
		return "converter";
	default:
		return "?";
	}
}

/**
 * format_name() - A pixel format's name.
 * @format: VCF_*
 *
 * Return: the name, "?" for one this does not know.
 */
const char *format_name(ULONG format)
{
	return format < sizeof(format_names) / sizeof(format_names[0]) ? format_names[format] : "?";
}

/**
 * format_of_name() - The pixel format a name gives, any case.
 * @name: the name, as format_name() gives it
 *
 * Return: VCF_*, VCF_Unknown for a name this does not know.
 */
ULONG format_of_name(const char *name)
{
	for (ULONG i = 1; i < sizeof(format_names) / sizeof(format_names[0]); i++)
		if (_Stricmp((CONST_STRPTR)name, (CONST_STRPTR)format_names[i]) == 0)
			return i;
	return VCF_Unknown;
}

/**
 * tag_name() - A FourCC as text.
 * @tag: the FourCC (VC_FOURCC())
 * @buf: room for TAG_NAME_SIZE characters
 *
 * Return: @buf, the four characters, a byte outside printable ASCII as '?'.
 */
char *tag_name(ULONG tag, char *buf)
{
	for (int i = 0; i < 4; i++)
	{
		UBYTE c = (UBYTE)(tag >> (24 - 8 * i));
		buf[i] = c >= ' ' && c <= '~' ? (char)c : '?';
	}
	buf[4] = '\0';
	return buf;
}

/**
 * tag_of_name() - A FourCC from text: up to four characters, blanks after a shorter one.
 * @name: the text
 *
 * Return: the FourCC, 0 for an empty or longer text.
 */
ULONG tag_of_name(const char *name)
{
	ULONG tag = 0;
	int i = 0;
	for (; i < 4 && name[i] != '\0'; i++)
		tag = (tag << 8) | (UBYTE)name[i];
	if (i == 0 || name[i] != '\0')
		return 0;
	for (; i < 4; i++)
		tag = (tag << 8) | (UBYTE)' ';
	return tag;
}
