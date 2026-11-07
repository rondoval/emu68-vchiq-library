/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocodec_private.h - videocodec.library internals.
 *
 * The frontend keeps a list of backends (libraries in LIBS:videocodec/, each
 * opened once, its codec table cached, the list in name order) and, per
 * session, which backend serves it.  Every public call is a tag parse at
 * most and one backend call; the backend serialises itself, so the only lock
 * here guards the backend list.
 */
#ifndef VIDEOCODEC_PRIVATE_H
#define VIDEOCODEC_PRIVATE_H

#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function, from base->SysBase */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/utility_protos.h>
#else
#include <proto/exec.h>
#include <proto/utility.h> /* calls go through a local UtilityBase, like SysBase */
#endif

#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/lists.h>

#include <types.h>
#include <debug.h>
#include <libraries/videocodec.h>
#include <libraries/videocodec_backend.h>

/* The backend's calls go through the generated inline macros with a local base of this name. */
#define VIDEOCODEC_BACKEND_BASE_NAME VCBackendBase
#ifdef __INTELLISENSE__
#include <clib/videocodec_backend_protos.h>
#else
#include <proto/videocodec_backend.h>
#endif

#if defined(__INTELLISENSE__)
#define asm(x)
#define __attribute__(x)
#endif

/* These are overridden by cmake */
#ifndef LIBRARY_NAME
#define LIBRARY_NAME "videocodec.library"
#endif
/* The version comes from CMake (the component's one version, root CMakeLists.txt); an editor's parser gets placeholders. */
#if !defined(LIBRARY_VERSION) || !defined(LIBRARY_REVISION) || !defined(LIBRARY_IDSTRING)
#ifdef __INTELLISENSE__
#ifndef LIBRARY_VERSION
#define LIBRARY_VERSION 0
#endif
#ifndef LIBRARY_REVISION
#define LIBRARY_REVISION 0
#endif
#ifndef LIBRARY_IDSTRING
#define LIBRARY_IDSTRING "videocodec.library"
#endif
#else
#error "LIBRARY_VERSION, LIBRARY_REVISION and LIBRARY_IDSTRING are set by CMake"
#endif
#endif
#ifndef LIBRARY_PRIORITY
#define LIBRARY_PRIORITY 0
#endif

#define VC_BACKEND_NAME_MAX 32                  /* a backend's file name, NUL included */
#define VC_FALLBACK_BACKEND "videocore.codec"   /* opened by name when the drawer cannot be scanned (a Task opener) */
#define VC_SESSION_MAGIC 0x56435345u            /* 'VCSE': marks a live struct VCSession */

/* One backend in the list: its library, kept open, and its codec table, read once. */
struct vc_backend
{
	struct MinNode node;            /* in VideoCodecBase.backends */
	struct Library *library;        /* the backend's base, for its VCB_* calls */
	struct VCCodecInfo *caps;       /* its codec table as it gave it (AllocVec); NULL if empty */
	ULONG ncaps;                    /* rows in caps */
	ULONG abi;                      /* VCInfo.ABIVersion as it reported it */
	char name[VC_BACKEND_NAME_MAX]; /* its file name in LIBS:videocodec/ */
};

struct vc_chain; /* a converter behind a decoder: videocodec_chain.c's own */

/* What a client's session pointer points to: which backend serves it and the backend's own handle. */
struct VCSession
{
	ULONG magic;                /* VC_SESSION_MAGIC while open; 0 once closed, so a stale pointer is refused */
	struct vc_backend *backend; /* the backend serving it (a chain's decoder), chosen at open */
	APTR handle;                /* the backend's session, passed to every VCB_* call */
	struct vc_chain *chain;     /* a converter behind the decoder, or NULL */
};

struct VideoCodecBase
{
	struct Library lib;
	ULONG segList;               /* returned at expunge */
	struct ExecBase *SysBase;    /* cached: $4 is a slow Chip-bus read */
	struct Library *UtilityBase; /* utility.library V36, open while the library is: tag lists */
	struct SignalSemaphore lock; /* guards backends and scanned */
	struct MinList backends;     /* struct vc_backend, in name order */
	BOOL scanned;                /* LIBS:videocodec/ was read; until then a Task opener gets only the fallback */
};

/* videocodec_backend.c: the backend list; all but init and free run with base->lock held */
void vc_backends_init(struct VideoCodecBase *base);   /* library init: empty list, not scanned */
void vc_backends_free(struct VideoCodecBase *base);   /* expunge: every backend closed */
void vc_backends_idle(struct VideoCodecBase *base);   /* the last opener gone: each backend lets go of what it keeps (VCB_Idle()) */
void vc_backends_load(struct VideoCodecBase *base); /* the drawer scanned if possible, else the fallback opened */
/* What a session, an image call or a chain asks of a backend's row (vc_backend_choose()) */
struct vc_choice
{
	ULONG codec, tag, kind; /* VCC_*, its FourCC or 0, VCK_* */
	ULONG use;              /* VCCF_Session or VCCF_Image */
	ULONG reads;            /* VCF_MASK()s of the pictures to read, any one of them; 0 asks nothing */
	ULONG writes;           /* VCF_MASK() of the picture to write; 0 asks nothing */
};
#define VC_FORMAT_BITS 32u           /* format numbers below this have a bit in a row's masks */
ULONG vc_format_mask(ULONG format); /* VCF_MASK(), 0 for none or past the masks' bits */
BOOL vc_row_takes(const struct VCCodecInfo *row, const struct vc_choice *want); /* reads and writes what is asked */
/* the backend of a session, an image call or a chain's converter, with the row that won */
struct vc_backend *vc_backend_choose(struct VideoCodecBase *base, const struct vc_choice *want, const char *name,
                                     const struct VCCodecInfo **row);

/* videocodec_tags.c */
ULONG vc_tag_fold(ULONG tag); /* a FourCC's letters in upper case */

/* videocodec_chain.c: a converter behind a decoder; chain_open() with base->lock held, the rest without (the backends serialise themselves) */
struct vc_chain *chain_open(struct VideoCodecBase *base, struct VCSessionParams *params, const struct vc_backend *decoder,
                            const struct VCCodecInfo *row, LONG *err); /* NULL with *err 0: none needed */
/* VCCodecInfo.ChainOutputFormats of a backend's row: what a chain behind its sessions adds; base->lock held */
ULONG chain_output_formats(struct VideoCodecBase *base, const struct vc_backend *decoder, const struct VCCodecInfo *row);
void chain_close(struct VideoCodecBase *base, struct VCSession *session); /* the converter and the chain; before the decoder's close */
LONG chain_poll(struct VCSession *session, struct VCItem *item);
struct VCFrame *chain_import_frame(struct VCSession *session, APTR data); /* an output frame: the converter's */
void chain_release_frame(struct VCSession *session, struct VCFrame *frame);
LONG chain_flush(struct VCSession *session);
LONG chain_set_attrs(struct VCSession *session, struct VCSessionParams *params);

/* the library's functions (LVOs, base in A6): documented in libraries/videocodec.h */
struct VCSession *VC_OpenSessionTagList(struct TagItem *tags asm("a0"), struct VideoCodecBase *base asm("a6"));
VOID VC_CloseSession(struct VCSession *session asm("a0"), struct VideoCodecBase *base asm("a6"));
struct VCPacket *VC_ImportPacket(struct VCSession *session asm("a0"), ULONG side asm("d0"), APTR data asm("a1"), ULONG capacity asm("d1"),
                                 struct VideoCodecBase *base asm("a6"));
struct VCFrame *VC_ImportFrame(struct VCSession *session asm("a0"), ULONG side asm("d0"), APTR data asm("a1"),
                               struct VideoCodecBase *base asm("a6"));
LONG VC_SendPacket(struct VCSession *session asm("a0"), struct VCPacket *packet asm("a1"), struct VideoCodecBase *base asm("a6"));
LONG VC_SendFrame(struct VCSession *session asm("a0"), struct VCFrame *frame asm("a1"), struct VideoCodecBase *base asm("a6"));
LONG VC_Poll(struct VCSession *session asm("a0"), struct VCItem *item asm("a1"), struct VideoCodecBase *base asm("a6"));
VOID VC_ReleaseFrame(struct VCSession *session asm("a0"), struct VCFrame *frame asm("a1"), struct VideoCodecBase *base asm("a6"));
VOID VC_ReleasePacket(struct VCSession *session asm("a0"), struct VCPacket *packet asm("a1"), struct VideoCodecBase *base asm("a6"));
LONG VC_Flush(struct VCSession *session asm("a0"), struct VideoCodecBase *base asm("a6"));
LONG VC_SetAttrsTagList(struct VCSession *session asm("a0"), struct TagItem *tags asm("a1"), struct VideoCodecBase *base asm("a6"));
LONG VC_QueryCodec(ULONG index asm("d0"), struct VCCodecInfo *info asm("a0"), struct VideoCodecBase *base asm("a6"));
ULONG VC_CodecOfTag(ULONG tag asm("d0"), struct VideoCodecBase *base asm("a6"));
LONG VC_GetBackendInfo(ULONG index asm("d0"), struct VCInfo *info asm("a0"), struct VideoCodecBase *base asm("a6"));
LONG VC_DecodeImageTagList(struct TagItem *tags asm("a0"), struct VideoCodecBase *base asm("a6"));
LONG VC_EncodeImageTagList(struct TagItem *tags asm("a0"), struct VideoCodecBase *base asm("a6"));
LONG VC_ConvertImageTagList(struct TagItem *tags asm("a0"), struct VideoCodecBase *base asm("a6"));
LONG VC_GetImageInfo(APTR data asm("a0"), ULONG length asm("d0"), struct VCImageInfo *info asm("a1"),
                     struct VideoCodecBase *base asm("a6"));

#endif /* VIDEOCODEC_PRIVATE_H */
