/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vcgrab.h - what VCGrab's files share.
 */
#ifndef VCGRAB_H
#define VCGRAB_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <intuition/screens.h>
#include <libraries/videocodec.h>

#include <bits.h>
#include <pixel_format.h>
#include <waiter.h>

extern struct Library *VideoCodecBase;
extern struct Library *P96Base;
extern struct IntuitionBase *IntuitionBase;

/* The I420 pictures' rows: the ISP's whole 64 pixels, whole 16 rows in the buffer */
#define I420_PITCH(width) ALIGN_UP(width, 64u)
#define I420_ROWS(height) ALIGN_UP(height, 16u)

/* A screen to take pictures of */
struct source
{
	struct Screen *screen;
	BOOL locked; /* by LockPubScreen() */
	const struct pixel_format *format;
	APTR memory; /* the bitmap in VRAM */
	ULONG width, height, pitch;
};

enum source_state
{
	SOURCE_SHOWN,  /* in front, where it was */
	SOURCE_HIDDEN, /* another screen is in front: VRAM holds that one */
	SOURCE_MOVED   /* in front, at another address */
};

BOOL source_find(struct source *src, const char *name);
void source_release(struct source *src);
enum source_state source_state(const struct source *src);

/* What one recording takes */
struct record_params
{
	const char *file;
	ULONG width, height; /* 0: the screen's */
	ULONG fps;
	ULONG bitrate; /* bits per second, 0: the backend's */
	ULONG seconds; /* 0: until stopped */
};

#define FPS_DEFAULT 30u
#define FPS_MAX 60u
#define KBIT_MAX 25000u /* level 4.0: the firmware refuses more */
#define SIDE_MIN 2u     /* I420 sizes are even; 0 would mean the source's */
#define SIDE_MAX 8192u  /* a picture's bytes stay well inside 32 bits */

enum hot_key
{
	HOT_START,
	HOT_STOP,
	HOT_STILL,
	HOT_COUNT
};

/* What the shell's arguments give, what ENV:VCGrab.prefs holds, what the window shows */
struct settings
{
	char to[256]; /* a file, no longer than a BSTR's 255; for the commodity the start of each take's name */
	ULONG width, height; /* 0: the screen's */
	ULONG fps;
	ULONG kbit;    /* 0: the backend's */
	ULONG seconds; /* 0: until stopped */
	char key[HOT_COUNT][64]; /* empty: none */
};

/* The room a setting takes: what holds a copy goes by this, not by a number of its own */
#define SETTING_ROOM(field) sizeof(((struct settings *)0)->field)

void settings_default(struct settings *s);
void settings_load(struct settings *s);
BOOL settings_save(const struct settings *s, BOOL archive);
const char *settings_check(const struct settings *s);
BOOL settings_size(const char *text, ULONG *width, ULONG *height);
BOOL settings_key(struct settings *s, enum hot_key which, const char *value);
void settings_split(const struct settings *s, char *drawer, char *name);
const char *settings_join(struct settings *s, const char *drawer, const char *name);
void settings_params(const struct settings *s, struct record_params *p, const char *file);

struct record;

void encoder_limits(ULONG *width, ULONG *height);
struct record *record_start(struct waiter *waiter, const struct source *src, const struct record_params *params);
ULONG record_pass(struct record *r, ULONG extra);
void record_stop(struct record *r);
BOOL record_over(const struct record *r);
int record_finish(struct record *r);

int still_take(ULONG width, ULONG height, const char *screen, const char *file);

/* The commodity, started from the shell or from Workbench */
struct commodity_args
{
	struct settings settings;
	char popkey[SETTING_ROOM(key[0])];
	LONG priority;
	BOOL popup;
};

#define POPKEY_DEFAULT "ctrl alt v"

int commodity(const struct commodity_args *args);

/* The window; commodity.c decides what its buttons mean */
enum window_action
{
	WINDOW_NOTHING,
	WINDOW_SAVE,
	WINDOW_USE,
	WINDOW_CANCEL,
	WINDOW_RECORD,
	WINDOW_STOP,
	WINDOW_STILL
};

BOOL window_show(const struct settings *s);
void window_hide(void);
void window_end(void);
ULONG window_sigmask(void);
enum window_action window_handle(void);
const char *window_read(struct settings *s);
void window_status(const char *format, ...);
void window_recording(BOOL recording);

/* What is said to the user: to the shell, and for grab_say() also the window's status line */
void grab_say(const char *format, ...);
void grab_report(const char *format, ...);
void grab_error(const char *what, LONG error);
void grab_fault(const char *name);

#endif /* VCGRAB_H */
