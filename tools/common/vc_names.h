/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vc_names.h - videocodec.library's codecs, kinds and pixel formats by name,
 * for the CLI tools (VCInfo, VCTest).
 */
#ifndef VC_NAMES_H
#define VC_NAMES_H

#include <exec/types.h>

const char *codec_name(ULONG codec);
const char *kind_name(ULONG kind);
const char *format_name(ULONG format);
#define FORMAT_BITS 32 /* the bits of a VCF_MASK() list: format numbers below this */
ULONG format_of_name(const char *name);
#define TAG_NAME_SIZE 5 /* a FourCC's four characters and the NUL */
char *tag_name(ULONG tag, char *buf);
ULONG tag_of_name(const char *name);

#endif /* VC_NAMES_H */
