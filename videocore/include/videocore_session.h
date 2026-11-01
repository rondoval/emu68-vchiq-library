/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_session.h - the sessions of the videocodec backend ABI on the
 * VPU's components (what the VCB_* entry points call), the codec table, and
 * what the still pipeline does (what the image calls ask).
 *
 * Every session call runs under session_lock, which the entry points take.
 * The session itself is opaque here; session_internal.h has its insides.
 */
#ifndef VIDEOCORE_SESSION_H
#define VIDEOCORE_SESSION_H

#include <exec/types.h>

#include <types.h>
#include <libraries/videocodec_backend.h>

struct VideoCoreBase;
struct vc_session;
struct image_facts;

/* videocore_session.c: the sessions */
/* At library init: no sessions */
void sessions_init(struct VideoCoreBase *base);
/* The reset guard's work: every session's components off the VPU, @user being the library base */
void sessions_reset(APTR user);
/* The session behind a handle; NULL if it is no open session */
struct vc_session *session_lookup(struct VideoCoreBase *base, APTR handle);
/* Open a session of the kind the parameters name; a still picture's only for an image call */
struct vc_session *session_open(struct VideoCoreBase *base, const struct VCSessionParams *params, BOOL image_call, LONG *error);
/* Close a session; @keep: an image call's that did its work keeps its components for the next call */
void session_close(struct vc_session *session, BOOL keep);
struct VCPacket *session_import_packet(struct vc_session *session, ULONG side, APTR data, ULONG capacity);
struct VCFrame *session_import_frame(struct vc_session *session, ULONG side, APTR data);
LONG session_send_packet(struct vc_session *session, struct VCPacket *packet);
LONG session_send_frame(struct vc_session *session, struct VCFrame *frame);
BOOL session_poll(struct vc_session *session, struct VCItem *item);
void session_release_frame(struct vc_session *session, struct VCFrame *frame);
void session_release_packet(struct vc_session *session, struct VCPacket *packet);
LONG session_flush(struct vc_session *session);
LONG session_set_attrs(struct vc_session *session, const struct VCSessionParams *changes);

/* videocore_codecs.c: what this backend does (no lock needed) */
/* One row of the codec table (VCB_QueryCodec()) */
LONG codec_query(ULONG index, struct VCCodecInfo *info);
/* The rows of the codec table */
ULONG codec_count(void);
/* Whether a codec is a still picture's: the image calls' only, never a client's session */
BOOL codec_still(ULONG codec);
/* Whether the image calls take a kind and codec (a VCCF_Image row) */
BOOL codec_image_allowed(ULONG kind, ULONG codec);

/* session_still.c: what the image decoder's pipeline does, for the image calls (videocore_image.c) */
/* Whether the pipeline decodes a picture right, by its header; else software must */
BOOL still_decodes(const struct image_facts *info);
/* Whether the pipeline can write the caller's picture itself: the picture's own size and a layout it writes */
BOOL still_direct(const struct image_facts *info, const struct VCImageSide *dest, ULONG keep_aspect);
BOOL still_strips(const struct image_facts *info);
/* What the pipeline writes for the converter after it: VCF_I420 or VCF_RGBA32 */
ULONG still_between(const struct image_facts *info);
/* Whether a decode fits one transfer per picture and the VPU's codec heap (the service is opened to tell) */
BOOL still_bounces(struct VideoCoreBase *base, const struct VCImageSide *dest, ULONG keep_aspect);
void still_bounce_copy(const struct VCFrame *frame, const struct VCImageSide *dest);
LONG still_fits(struct VideoCoreBase *base, const struct image_facts *info, const struct VCImageSide *dest, BOOL direct, ULONG keep_aspect);
void still_frame_release(struct VideoCoreBase *base);

#endif /* VIDEOCORE_SESSION_H */
