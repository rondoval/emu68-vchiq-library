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
ULONG format_of_name(const char *name);

#endif /* VC_NAMES_H */
