/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * VCGrab - records an RTG screen through the VideoCore, the 68k only pacing
 * and writing the file, and grabs stills.
 *
 *   VCGrab TO [SCREEN name] [SIZE WxH] [FPS n] [BITRATE kbit/s] [SECONDS n]
 *   VCGrab TO STILL [SCREEN name] [SIZE WxH]
 *   VCGrab [TO] CX [SIZE WxH] [FPS n] [BITRATE kbit/s] [SECONDS n]
 *          [STARTKEY key] [STOPKEY key] [STILLKEY key]
 *          [CX_POPKEY key] [CX_POPUP YES|NO] [CX_PRIORITY n]
 *
 * The first records TO, an MKV file, until SECONDS or Ctrl-C; the second
 * writes TO, a JPEG.  The screen is the one in front, or the public screen
 * named, which must be in front too.  While it is not in front the recording
 * pauses.  A file of that name is replaced only once the recording can start.
 *
 * CX stays in the background as a commodity (commodity.c), as a start from
 * Workbench always does, its icon's tooltypes being CX_POPKEY, CX_POPUP and
 * CX_PRIORITY.  Its hot keys start and stop a recording and grab a still of
 * the screen in front then, and a window (window.c) holds its settings.  TO
 * is the start of each file's name: RAM:grab gives RAM:grab-0001.mkv,
 * RAM:grab-0002.jpg, the first free number.  It starts from what
 * ENV:VCGrab.prefs says (settings.c), arguments given over that for the run;
 * the first two forms ignore that file.  Start it with Run >NIL:.
 *
 * Return codes: 0 done, 10 the screen, a library, memory or the file
 * refused, 20 the arguments made no sense or it failed on the way.
 *
 * Hosted program (libnix).
 */
#ifdef __INTELLISENSE__
#include <clib/dos_protos.h>
#include <clib/exec_protos.h>
#include <clib/icon_protos.h>
#include <clib/intuition_protos.h>
#else
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/intuition.h>
#endif

#include <dos/dos.h>
#include <dos/rdargs.h>
#include <intuition/intuition.h>
#include <libraries/Picasso96.h>
#include <stdarg.h>
#include <string.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>

#include <args.h>
#include <format.h>
#include <strutil.h>

#include "vcgrab.h"

struct Library *VideoCodecBase;
struct Library *P96Base;
struct IntuitionBase *IntuitionBase;
struct Library *IconBase;

/* libnix swaps to a stack of this size: the library's calls and waits run on it */
unsigned long __stack = 32768;

static const char verstag[] __attribute__((used)) = VERSTAG;

static const char arg_template[] = "TO,SCREEN/K,SIZE/K,FPS/K/N,BITRATE/K/N,SECONDS/K/N,STILL/S,CX/S,STARTKEY/K,STOPKEY/K,STILLKEY/K,"
                                   "CX_POPKEY/K,CX_POPUP/K,CX_PRIORITY/K/N";

enum
{
	ARG_TO,
	ARG_SCREEN,
	ARG_SIZE,
	ARG_FPS,
	ARG_BITRATE,
	ARG_SECONDS,
	ARG_STILL,
	ARG_CX,
	ARG_STARTKEY,
	ARG_STOPKEY,
	ARG_STILLKEY,
	ARG_POPKEY,
	ARG_POPUP,
	ARG_PRIORITY,
	ARG_COUNT
};

/* Printing goes by this, not by Output(): under Workbench libnix selects a
 * console that opens at the first write and waits to be closed at the end. */
static BOOL from_workbench;
static char said[200];

/* ---- what is said ----------------------------------------------------------------- */

/**
 * grab_say() - Say what went wrong or what happened instead, to the shell and the window.
 * @format: its format, for _VSNPrintf()
 */
void grab_say(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	_VSNPrintf((STRPTR)said, sizeof(said), (CONST_STRPTR)format, args);
	va_end(args);
	if (!from_workbench)
		Printf((CONST_STRPTR) "VCGrab: %s\n", said);
	window_status("%s", said);
}

/**
 * grab_report() - Tell the shell how something went; the window gets its own, shorter line.
 * @format: its format, for _VSNPrintf()
 */
void grab_report(const char *format, ...)
{
	char line[sizeof(said)];
	va_list args;
	va_start(args, format);
	_VSNPrintf((STRPTR)line, sizeof(line), (CONST_STRPTR)format, args);
	va_end(args);
	if (!from_workbench)
		Printf((CONST_STRPTR) "%s\n", line);
}

/**
 * grab_error() - Say what failed; the errors a user can act on say what to do about them.
 * @what:  what failed
 * @error: the error
 */
void grab_error(const char *what, LONG error)
{
	if (error == VCERR_DEVMEM)
		grab_say("%s: the VideoCore is out of memory, raise gpu_mem in config.txt", what);
	else if (error == VCERR_TRANSPORT)
		grab_say("%s: no link to the VideoCore, VCInfo says why", what);
	else
		grab_say("%s: error %ld", what, error);
}

/**
 * grab_fault() - Say why a file failed, from IoErr().
 * @name: the file
 */
void grab_fault(const char *name)
{
	char text[sizeof(said)];
	Fault(IoErr(), (CONST_STRPTR)name, (STRPTR)text, sizeof(text));
	grab_say("%s", text);
}

/**
 * workbench_tell() - Show a failure that ends a Workbench start.
 *
 * Only here: a requester waits for an answer, which a recording cannot.
 */
static void workbench_tell(void)
{
	if (IntuitionBase == NULL || said[0] == 0)
		return;
	struct EasyStruct es = {sizeof(struct EasyStruct), 0, (STRPTR) "VCGrab", (STRPTR) "%s", (STRPTR) "OK"};
	APTR args[1] = {said};
	(void)EasyRequestArgs(NULL, &es, NULL, (APTR)args);
}

/* ---- the arguments ---------------------------------------------------------------- */

/**
 * key_too_long() - Say that a hot key does not fit.
 */
static void key_too_long(void)
{
	grab_say("A hot key is at most %lu characters", (ULONG)SETTING_ROOM(key[0]) - 1u);
}

/**
 * args_read() - Take the arguments given over what @s holds.
 * @args: ReadArgs()'s array
 * @s:    the settings
 *
 * Return: TRUE, FALSE with the reason said.
 */
static BOOL args_read(const LONG *args, struct settings *s)
{
	const char *to = arg_str(args, ARG_TO);
	if (to != NULL && !strfit(s->to, to, sizeof(s->to)))
	{
		grab_say("TO is at most %lu characters", (ULONG)sizeof(s->to) - 1u);
		return FALSE;
	}

	const char *size = arg_str(args, ARG_SIZE);
	if (size != NULL && !settings_size(size, &s->width, &s->height))
	{
		grab_say("SIZE is written like 1280x720, each side %lu to %lu", (ULONG)SIDE_MIN, (ULONG)SIDE_MAX);
		return FALSE;
	}

	s->fps = arg_num(args, ARG_FPS, s->fps);
	if (s->fps == 0 || s->fps > FPS_MAX)
	{
		grab_say("FPS is 1 to %lu", (ULONG)FPS_MAX);
		return FALSE;
	}

	s->kbit = arg_num(args, ARG_BITRATE, s->kbit);
	if (s->kbit > KBIT_MAX)
	{
		grab_say("BITRATE is at most %lu: the encoder's level 4.0 takes no more", (ULONG)KBIT_MAX);
		return FALSE;
	}

	s->seconds = arg_num(args, ARG_SECONDS, s->seconds);
	for (ULONG i = 0; i < HOT_COUNT; i++)
	{
		const char *key = arg_str(args, ARG_STARTKEY + i);
		if (key != NULL && !settings_key(s, (enum hot_key)i, key))
		{
			key_too_long();
			return FALSE;
		}
	}

	return TRUE;
}

/**
 * recording() - Record from the shell, until SECONDS or Ctrl-C.
 * @s:      what to record
 * @screen: a public screen's name, NULL for the screen in front
 *
 * Return: the return code.
 */
static int recording(const struct settings *s, const char *screen)
{
	struct waiter waiter;
	memset(&waiter, 0, sizeof(waiter));
	if (!waiter_open(&waiter))
		return RETURN_ERROR;
	struct record_params p;
	settings_params(s, &p, s->to);
	int rc = RETURN_ERROR;
	struct source src;
	if (source_find(&src, screen))
	{
		struct record *r = record_start(&waiter, &src, &p);
		if (r != NULL)
		{
			while (!record_over(r))
				if (record_pass(r, 0) & SIGBREAKF_CTRL_C)
					record_stop(r);
			rc = record_finish(r);
		}
		source_release(&src);
	}
	waiter_close(&waiter);
	return rc;
}

/**
 * one_shot() - Do what the first two forms ask for: the saved settings play no part, so a script does what it says.
 * @args: ReadArgs()'s array
 *
 * Return: the return code.
 */
static int one_shot(const LONG *args)
{
	struct settings s;
	settings_default(&s);
	s.to[0] = 0;
	if (!args_read(args, &s))
		return RETURN_FAIL;
	if (s.to[0] == 0)
	{
		grab_say("TO names the file");
		return RETURN_FAIL;
	}
	if (args[ARG_STILL] != 0)
		return still_take(s.width, s.height, arg_str(args, ARG_SCREEN), s.to);
	return recording(&s, arg_str(args, ARG_SCREEN));
}

/**
 * commodity_args_init() - Fill in how the commodity starts with nothing asked for.
 * @a: where it goes
 */
static void commodity_args_init(struct commodity_args *a)
{
	settings_default(&a->settings);
	settings_load(&a->settings);
	strlcpy(a->popkey, POPKEY_DEFAULT, sizeof(a->popkey));
	a->priority = 0;
	a->popup = FALSE;
}

/**
 * popup_of() - Read CX_POPUP's value.
 * @value: its text
 *
 * Return: TRUE for YES, any case.
 */
static BOOL popup_of(const char *value)
{
	return _Stricmp((CONST_STRPTR)value, (CONST_STRPTR) "YES") == 0;
}

/**
 * priority_of() - Fit CX_PRIORITY to the broker's byte.
 * @value: what was asked for
 *
 * Return: the priority.
 */
static LONG priority_of(LONG value)
{
	return value < -128 ? -128 : value > 127 ? 127 : value;
}

/**
 * shell_commodity() - Start the commodity from the shell: without CX_POPUP it stays out of sight, as a script wants.
 * @args: ReadArgs()'s array
 *
 * Return: the return code.
 */
static int shell_commodity(const LONG *args)
{
	struct commodity_args a;
	commodity_args_init(&a);
	if (!args_read(args, &a.settings))
		return RETURN_FAIL;
	if (arg_str(args, ARG_POPKEY) != NULL && !strfit(a.popkey, arg_str(args, ARG_POPKEY), sizeof(a.popkey)))
	{
		key_too_long();
		return RETURN_FAIL;
	}
	if (arg_str(args, ARG_POPUP) != NULL)
		a.popup = popup_of(arg_str(args, ARG_POPUP));
	if (args[ARG_PRIORITY] != 0)
		a.priority = priority_of(*(const LONG *)args[ARG_PRIORITY]);
	return commodity(&a);
}

/**
 * shell() - Start from the shell.
 *
 * Return: the return code.
 */
static int shell(void)
{
	LONG args[ARG_COUNT];
	memset(args, 0, sizeof(args));
	struct RDArgs *rda = ReadArgs((CONST_STRPTR)arg_template, args, NULL);
	if (rda == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "VCGrab");
		return RETURN_FAIL;
	}
	int rc = args[ARG_CX] != 0 && args[ARG_STILL] == 0 ? shell_commodity(args) : one_shot(args);
	FreeArgs(rda);
	return rc;
}

/**
 * tooltypes_read() - Take the icon's tooltypes.
 * @wbs: the start message
 * @a:   how the commodity starts
 */
static void tooltypes_read(const struct WBStartup *wbs, struct commodity_args *a)
{
	if (IconBase == NULL || wbs->sm_NumArgs < 1)
		return;

	const struct WBArg *arg = &wbs->sm_ArgList[0];
	BPTR old = CurrentDir(arg->wa_Lock);
	struct DiskObject *icon = GetDiskObject(arg->wa_Name);
	(void)CurrentDir(old);
	if (icon == NULL)
		return;

	CONST_STRPTR *types = (CONST_STRPTR *)icon->do_ToolTypes;
	const char *value = (const char *)FindToolType(types, (CONST_STRPTR) "CX_POPKEY");
	if (value != NULL)
		(void)strfit(a->popkey, value, sizeof(a->popkey));

	value = (const char *)FindToolType(types, (CONST_STRPTR) "CX_POPUP");
	if (value != NULL)
		a->popup = popup_of(value);

	value = (const char *)FindToolType(types, (CONST_STRPTR) "CX_PRIORITY");
	LONG priority;
	if (value != NULL && StrToLong((CONST_STRPTR)value, &priority) > 0)
		a->priority = priority_of(priority);

	FreeDiskObject(icon);
}

/**
 * workbench() - Start from Workbench: the icon can only mean the commodity.
 * @wbs: the start message
 *
 * Return: the return code.
 */
static int workbench(const struct WBStartup *wbs)
{
	struct commodity_args a;
	commodity_args_init(&a);
	a.popup = TRUE;
	IconBase = OpenLibrary((CONST_STRPTR) "icon.library", 37);
	tooltypes_read(wbs, &a);
	if (IconBase != NULL)
		CloseLibrary(IconBase);
	return commodity(&a);
}

int main(int argc, char **argv)
{
	from_workbench = argc == 0;
	int rc = RETURN_ERROR;
	IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR) "intuition.library", 39);
	P96Base = OpenLibrary((CONST_STRPTR)P96NAME, 2);
	VideoCodecBase = OpenLibrary((CONST_STRPTR)VIDEOCODEC_NAME, 0);
	if (IntuitionBase == NULL || P96Base == NULL || VideoCodecBase == NULL)
		grab_say("%s is missing", P96Base == NULL ? "Picasso96API.library" : VideoCodecBase == NULL ? VIDEOCODEC_NAME : "intuition.library");
	else
		rc = from_workbench ? workbench((const struct WBStartup *)argv) : shell();
	if (from_workbench && rc != RETURN_OK)
		workbench_tell();
	if (VideoCodecBase != NULL)
		CloseLibrary(VideoCodecBase);
	if (P96Base != NULL)
		CloseLibrary(P96Base);
	if (IntuitionBase != NULL)
		CloseLibrary((struct Library *)IntuitionBase);
	return rc;
}
