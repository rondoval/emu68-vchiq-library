/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * commands.h - VCTest's commands: what they share, and each one's template
 * and entry point.  A command reads its own arguments by an enum that
 * matches its template (one file each), prints its facts as key=value
 * (kv.h) and returns an AmigaDOS code:
 *
 *   RETURN_OK     done, and the command's own checks passed
 *   RETURN_WARN   done, a self-check failed (fail=)
 *   RETURN_ERROR  the environment: a library, device, file or memory
 *                 missing, or no test hooks (fail=)
 *   RETURN_FAIL   the API refused or failed (error=), nothing happened for
 *                 IDLE_MS, or the arguments made no sense (fail=)
 */
#ifndef VCTEST_COMMANDS_H
#define VCTEST_COMMANDS_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <utility/tagitem.h>
#include <libraries/videocodec.h>

#include <waiter.h>
#include <vchiq_client.h>

extern struct Library *VideoCodecBase;

#define IDLE_MS 5000 /* nothing moved for this long: stuck */
#define ARGS_MAX 24  /* the longest template's entries; parse_rest() refuses a longer one */

/* What every command gets */
struct tool
{
	struct waiter waiter;
	struct vq_service svc;
	const char *backend; /* BACKEND: the backend every session and image call asks for, NULL for the library's choice */
	BOOL misalign;       /* MISALIGN: pictures 4 bytes past a cache line, which the backend then sends by bulk */
};

/* VCA_Backend when BACKEND was given, else nothing: two tag slots */
#define BACKEND_TAG(t) ((t)->backend != NULL ? VCA_Backend : TAG_IGNORE), (ULONG)(t)->backend

typedef int (*command_fn)(struct tool *t, LONG *args);

extern const char status_template[];
extern const char codecs_template[];
extern const char abandon_template[];
extern const char audio_template[];
extern const char ahiwalk_template[];
extern const char ahitone_template[];
extern const char decode_template[];
extern const char image_template[];
extern const char tojpeg_template[];
extern const char convert_template[];
extern const char encode_template[];
extern const char datatype_template[];
extern const char page_template[];
extern const char screen_template[];
extern const char hold_template[];

int do_status(struct tool *t, LONG *args);
int do_codecs(struct tool *t, LONG *args);
int do_abandon(struct tool *t, LONG *args);
int do_audio(struct tool *t, LONG *args);
int do_ahiwalk(struct tool *t, LONG *args);
int do_ahitone(struct tool *t, LONG *args);
int do_decode(struct tool *t, LONG *args);
int do_image(struct tool *t, LONG *args);
int do_tojpeg(struct tool *t, LONG *args);
int do_convert(struct tool *t, LONG *args);
int do_encode(struct tool *t, LONG *args);
int do_datatype(struct tool *t, LONG *args);
int do_page(struct tool *t, LONG *args);
int do_screen(struct tool *t, LONG *args);
int do_hold(struct tool *t, LONG *args);

#endif /* VCTEST_COMMANDS_H */
