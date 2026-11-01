/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vc_names.c - videocodec.library's codecs, kinds and pixel formats by name.
 */
#include <libraries/videocodec.h>
#include <strutil.h>

#include "vc_names.h"

/* VCF_* by number */
static const char *const format_names[] = {"?", "I420", "YV12", "NV12", "RGB16", "RGB24", "BGR24", "BGRA32", "RGBA32", "YUYV", "UYVY"};

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
