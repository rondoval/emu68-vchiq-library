/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * settings.c - what a recording is asked to be, and the commodity's copy of
 * it in ENV:VCGrab.prefs: one KEY = VALUE a line, the keys those of the shell.
 * A value that makes no sense leaves the default.  Save writes ENVARC: too,
 * which the next boot copies to ENV:.
 */
#ifdef __INTELLISENSE__
#include <clib/dos_protos.h>
#include <clib/exec_protos.h>
#else
#include <proto/dos.h>
#include <proto/exec.h>
#endif

#include <dos/dos.h>
#include <stdlib.h>
#include <string.h>

#include <prefs.h>
#include <strutil.h>

#include "vcgrab.h"

#define PREFS_ENV "ENV:VCGrab.prefs"
#define PREFS_ENVARC "ENVARC:VCGrab.prefs"
#define TO_DEFAULT "RAM:grab"
#define KEY_OFF "NONE"

static const char *const key_names[HOT_COUNT] = {"STARTKEY", "STOPKEY", "STILLKEY"};
static const char *const key_defaults[HOT_COUNT] = {"ctrl alt r", "ctrl alt s", "ctrl alt g"};

/**
 * settings_key() - Set a hot key; "NONE" turns it off.
 * @s:     the settings
 * @which: the key
 * @value: its description
 *
 * Return: TRUE, FALSE with the key left as it was if @value does not fit.
 */
BOOL settings_key(struct settings *s, enum hot_key which, const char *value)
{
	return strfit(s->key[which], _Stricmp((CONST_STRPTR)value, (CONST_STRPTR)KEY_OFF) == 0 ? "" : value, sizeof(s->key[which]));
}

/**
 * settings_default() - Fill in what holds with nothing saved.
 * @s: the settings
 */
void settings_default(struct settings *s)
{
	memset(s, 0, sizeof(*s));
	strlcpy(s->to, TO_DEFAULT, sizeof(s->to));
	s->fps = FPS_DEFAULT;
	for (ULONG i = 0; i < HOT_COUNT; i++)
		(void)settings_key(s, (enum hot_key)i, key_defaults[i]);
}

/**
 * settings_size() - Read a size written WIDTHxHEIGHT.
 * @text:   the text
 * @width:  where the width goes
 * @height: where the height goes
 *
 * Return: TRUE when it is one, each side within SIDE_MIN and SIDE_MAX.
 */
BOOL settings_size(const char *text, ULONG *width, ULONG *height)
{
	char *end;
	ULONG w = strtoul(text, &end, 10);
	if (end == text || (*end != 'x' && *end != 'X'))
		return FALSE;
	const char *second = end + 1;
	ULONG h = strtoul(second, &end, 10);
	if (end == second || *end != 0 || w < SIDE_MIN || w > SIDE_MAX || h < SIDE_MIN || h > SIDE_MAX)
		return FALSE;
	*width = w;
	*height = h;
	return TRUE;
}

/**
 * setting_line() - Take one line of the file; a key not known is left alone.
 * @s:     the settings
 * @key:   its key
 * @value: its value
 *
 * A relative TO would mean the tool's own drawer under Workbench.
 */
static void setting_line(struct settings *s, const char *key, const char *value)
{
	CONST_STRPTR name = (CONST_STRPTR)key;
	if (_Stricmp(name, (CONST_STRPTR) "TO") == 0)
	{
		if (strchr(value, ':') != NULL)
			(void)strfit(s->to, value, sizeof(s->to));
	}
	else if (_Stricmp(name, (CONST_STRPTR) "SIZE") == 0)
		(void)settings_size(value, &s->width, &s->height);
	else if (_Stricmp(name, (CONST_STRPTR) "FPS") == 0)
		(void)prefs_number(value, 1, FPS_MAX, (LONG *)&s->fps);
	else if (_Stricmp(name, (CONST_STRPTR) "BITRATE") == 0)
		(void)prefs_number(value, 0, KBIT_MAX, (LONG *)&s->kbit);
	else if (_Stricmp(name, (CONST_STRPTR) "SECONDS") == 0)
		(void)prefs_number(value, 0, PREFS_NUMBER_MOST, (LONG *)&s->seconds);
	else
		for (ULONG i = 0; i < HOT_COUNT; i++)
			if (_Stricmp(name, (CONST_STRPTR)key_names[i]) == 0)
				(void)settings_key(s, (enum hot_key)i, value);
}

/**
 * settings_load() - Take what ENV:VCGrab.prefs says over what @s holds.
 * @s: the settings
 */
void settings_load(struct settings *s)
{
	struct prefs_file f;
	if (!prefs_open(&f, SysBase, PREFS_ENV))
		return;
	char *key, *value;
	while (prefs_next(&f, &key, &value))
		setting_line(s, key, value);
	prefs_close(&f);
}

/**
 * prefs_write() - Write the settings to one file.
 * @path: the file
 * @s:    the settings
 *
 * FPrintf() is buffered, so a disk that fills up may only show at Close().
 *
 * Return: TRUE, FALSE with the reason said.
 */
static BOOL prefs_write(const char *path, const struct settings *s)
{
	BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
	if (fh == 0)
	{
		grab_fault(path);
		return FALSE;
	}
	LONG written = FPrintf(fh, (CONST_STRPTR) "TO = %s\n", s->to); /* -1 on failure: it stays negative through the ORs */
	if (s->width != 0)
		written |= FPrintf(fh, (CONST_STRPTR) "SIZE = %lux%lu\n", s->width, s->height);
	written |= FPrintf(fh, (CONST_STRPTR) "FPS = %lu\nBITRATE = %lu\nSECONDS = %lu\n", s->fps, s->kbit, s->seconds);
	for (ULONG i = 0; i < HOT_COUNT; i++)
		written |= FPrintf(fh, (CONST_STRPTR) "%s = %s\n", key_names[i], s->key[i][0] != 0 ? s->key[i] : KEY_OFF);
	if (!Close(fh) || written < 0)
	{
		grab_fault(path);
		return FALSE;
	}
	return TRUE;
}

/**
 * settings_save() - Write the settings for this boot, and for good if asked.
 * @s:       the settings
 * @archive: TRUE for ENVARC: too
 *
 * Return: TRUE, FALSE with the reason said.
 */
BOOL settings_save(const struct settings *s, BOOL archive)
{
	BOOL saved = prefs_write(PREFS_ENV, s);
	if (saved && archive)
		saved = prefs_write(PREFS_ENVARC, s);
	return saved;
}

/**
 * settings_check() - Find what the window must not accept.
 * @s: the settings
 *
 * Return: NULL, or why not in plain words.
 */
const char *settings_check(const struct settings *s)
{
	if (strchr(s->to, ':') == NULL)
		return "The drawer needs a volume, like RAM:";
	const char *start = s->key[HOT_START], *stop = s->key[HOT_STOP];
	if (*start == 0)
		return NULL;
	if (_Stricmp((CONST_STRPTR)start, (CONST_STRPTR)stop) == 0)
		return "Start and Stop need different keys";
	if (*stop == 0 && s->seconds == 0)
		return "A Start key needs a Stop key or a time limit";
	return NULL;
}

/**
 * settings_split() - Cut TO into what the window shows.
 * @s:      the settings
 * @drawer: where the drawer goes, SETTING_ROOM(to) bytes
 * @name:   where the name goes, SETTING_ROOM(to) bytes
 */
void settings_split(const struct settings *s, char *drawer, char *name)
{
	ULONG length = (ULONG)((const char *)PathPart((CONST_STRPTR)s->to) - s->to);
	memcpy(drawer, s->to, length);
	drawer[length] = 0;
	strlcpy(name, (const char *)FilePart((CONST_STRPTR)s->to), SETTING_ROOM(to));
}

/**
 * settings_join() - Make TO of what the window shows.
 * @s:      the settings
 * @drawer: the drawer
 * @name:   the name
 *
 * Return: NULL, or why not in plain words.
 */
const char *settings_join(struct settings *s, const char *drawer, const char *name)
{
	if (*name == 0 || strchr(name, ':') != NULL || strchr(name, '/') != NULL)
		return "The name needs some text, and no : or /";
	if (strfit(s->to, drawer, sizeof(s->to)) && AddPart((STRPTR)s->to, (CONST_STRPTR)name, sizeof(s->to)))
		return NULL;
	return "The drawer and the name are too long";
}

/**
 * settings_params() - Fill in what one recording takes.
 * @s:    the settings
 * @p:    what it takes
 * @file: its file, valid while it runs
 */
void settings_params(const struct settings *s, struct record_params *p, const char *file)
{
	p->file = file;
	p->width = s->width;
	p->height = s->height;
	p->fps = s->fps;
	p->bitrate = s->kbit * 1000u;
	p->seconds = s->seconds;
}
