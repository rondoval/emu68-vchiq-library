/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocodec_tags.c - FourCCs: VC_CodecOfTag(), which tells a program the
 * codec of the FourCC its container names, and the comparison the backend
 * choice uses for VCA_CodecTag.  One table for every program, so that no
 * player keeps its own.  Case does not count: files write xvid and XVID,
 * hvc1 and HVC1 alike.
 */
#include <videocodec_private.h>

/* A FourCC and its codec, the FourCC with its letters in upper case */
struct vc_tag_codec
{
	ULONG tag;
	ULONG codec;
};

/* The FourCCs of AVI's biCompression, MP4 and QuickTime sample entries and Matroska's V_MS/VFW/FOURCC */
static const struct vc_tag_codec tag_codecs[] = {
    {VC_FOURCC('A', 'V', 'C', '1'), VCC_H264},    {VC_FOURCC('A', 'V', 'C', '3'), VCC_H264},
    {VC_FOURCC('H', '2', '6', '4'), VCC_H264},    {VC_FOURCC('X', '2', '6', '4'), VCC_H264},
    {VC_FOURCC('D', 'A', 'V', 'C'), VCC_H264},    {VC_FOURCC('H', 'V', 'C', '1'), VCC_H265},
    {VC_FOURCC('H', 'E', 'V', '1'), VCC_H265},    {VC_FOURCC('H', 'E', 'V', 'C'), VCC_H265},
    {VC_FOURCC('H', '2', '6', '5'), VCC_H265},    {VC_FOURCC('X', '2', '6', '5'), VCC_H265},
    {VC_FOURCC('M', 'J', 'P', 'G'), VCC_MJPEG},   {VC_FOURCC('J', 'P', 'E', 'G'), VCC_MJPEG},
    {VC_FOURCC('M', 'J', 'P', 'A'), VCC_MJPEG},   {VC_FOURCC('M', 'J', 'P', 'B'), VCC_MJPEG},
    {VC_FOURCC('A', 'V', 'R', 'N'), VCC_MJPEG},   {VC_FOURCC('D', 'M', 'B', '1'), VCC_MJPEG},
    {VC_FOURCC('M', 'P', 'G', '1'), VCC_MPEG1},   {VC_FOURCC('P', 'I', 'M', '1'), VCC_MPEG1},
    {VC_FOURCC('M', '1', 'V', ' '), VCC_MPEG1},   {VC_FOURCC('M', 'P', 'G', '2'), VCC_MPEG2},
    {VC_FOURCC('M', 'P', '2', 'V'), VCC_MPEG2},   {VC_FOURCC('M', '2', 'V', '1'), VCC_MPEG2},
    {VC_FOURCC('P', 'I', 'M', '2'), VCC_MPEG2},   {VC_FOURCC('H', 'D', 'V', '1'), VCC_MPEG2},
    {VC_FOURCC('H', 'D', 'V', '2'), VCC_MPEG2},   {VC_FOURCC('H', 'D', 'V', '3'), VCC_MPEG2},
    {VC_FOURCC('H', 'D', 'V', '5'), VCC_MPEG2},   {VC_FOURCC('H', 'D', 'V', '6'), VCC_MPEG2},
    {VC_FOURCC('M', 'P', '4', 'V'), VCC_MPEG4},   {VC_FOURCC('X', 'V', 'I', 'D'), VCC_MPEG4},
    {VC_FOURCC('D', 'I', 'V', 'X'), VCC_MPEG4},   {VC_FOURCC('D', 'X', '5', '0'), VCC_MPEG4},
    {VC_FOURCC('F', 'M', 'P', '4'), VCC_MPEG4},   {VC_FOURCC('3', 'I', 'V', '2'), VCC_MPEG4},
    {VC_FOURCC('M', '4', 'S', '2'), VCC_MPEG4},   {VC_FOURCC('R', 'M', 'P', '4'), VCC_MPEG4},
    {VC_FOURCC('B', 'L', 'Z', '0'), VCC_MPEG4},   {VC_FOURCC('S', 'E', 'D', 'G'), VCC_MPEG4},
    {VC_FOURCC('M', 'P', 'G', '4'), VCC_MSMPEG4}, {VC_FOURCC('M', 'P', '4', '1'), VCC_MSMPEG4},
    {VC_FOURCC('M', 'P', '4', '2'), VCC_MSMPEG4}, {VC_FOURCC('D', 'I', 'V', '2'), VCC_MSMPEG4},
    {VC_FOURCC('M', 'P', '4', '3'), VCC_MSMPEG4}, {VC_FOURCC('D', 'I', 'V', '3'), VCC_MSMPEG4},
    {VC_FOURCC('D', 'I', 'V', '4'), VCC_MSMPEG4}, {VC_FOURCC('D', 'I', 'V', '5'), VCC_MSMPEG4},
    {VC_FOURCC('D', 'I', 'V', '6'), VCC_MSMPEG4}, {VC_FOURCC('A', 'P', '4', '1'), VCC_MSMPEG4},
    {VC_FOURCC('H', '2', '6', '3'), VCC_H263},    {VC_FOURCC('S', '2', '6', '3'), VCC_H263},
    {VC_FOURCC('U', '2', '6', '3'), VCC_H263},    {VC_FOURCC('I', '2', '6', '3'), VCC_H263},
    {VC_FOURCC('F', 'L', 'V', '1'), VCC_H263},    {VC_FOURCC('V', 'P', '8', '0'), VCC_VP8},
    {VC_FOURCC('V', 'P', '0', '8'), VCC_VP8},     {VC_FOURCC('V', 'P', '9', '0'), VCC_VP9},
    {VC_FOURCC('V', 'P', '0', '9'), VCC_VP9},     {VC_FOURCC('A', 'V', '0', '1'), VCC_AV1},
    {VC_FOURCC('W', 'V', 'C', '1'), VCC_VC1},     {VC_FOURCC('W', 'M', 'V', '3'), VCC_VC1},
    {VC_FOURCC('W', 'M', 'V', 'A'), VCC_VC1},     {VC_FOURCC('V', 'C', '-', '1'), VCC_VC1},
    {VC_FOURCC('T', 'H', 'E', 'O'), VCC_Theora},  {VC_FOURCC('T', 'H', 'R', 'A'), VCC_Theora},
    {VC_FOURCC('C', 'V', 'I', 'D'), VCC_Cinepak}, {VC_FOURCC('C', 'R', 'A', 'M'), VCC_MSVideo1},
    {VC_FOURCC('M', 'S', 'V', 'C'), VCC_MSVideo1}, {VC_FOURCC('W', 'H', 'A', 'M'), VCC_MSVideo1},
    {VC_FOURCC('R', 'L', 'E', ' '), VCC_QTRLE},
};

/**
 * vc_tag_fold() - A FourCC with its letters in upper case.
 * @tag: the FourCC
 *
 * Return: @tag, each byte from 'a' to 'z' made upper case.
 */
ULONG vc_tag_fold(ULONG tag)
{
	ULONG folded = 0;
	for (int shift = 24; shift >= 0; shift -= 8)
	{
		UBYTE c = (UBYTE)(tag >> shift);
		if (c >= 'a' && c <= 'z')
			c = (UBYTE)(c - 'a' + 'A');
		folded |= (ULONG)c << shift;
	}
	return folded;
}

/**
 * VC_CodecOfTag() - The codec a FourCC names (LVO).
 * @tag:  the FourCC as the container holds it (VC_FOURCC()), in any case
 * @base: the library base (A6)
 *
 * Return: the VCC_*, or VCC_None for a FourCC the library does not know.
 */
ULONG VC_CodecOfTag(ULONG tag asm("d0"), struct VideoCodecBase *base asm("a6"))
{
	(void)base;
	ULONG want = vc_tag_fold(tag);
	for (ULONG i = 0; i < sizeof(tag_codecs) / sizeof(tag_codecs[0]); i++)
		if (tag_codecs[i].tag == want)
			return tag_codecs[i].codec;
	return VCC_None;
}
