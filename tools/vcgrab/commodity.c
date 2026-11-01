/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * commodity.c - VCGrab in the background: the broker, its hot keys, the
 * window's buttons, and a recording going on between them.  One task: the
 * hot keys and the window are served between a recording's passes, so
 * nothing here may block for long while one runs.
 *
 * Settings changed during a recording hold from the next take: it runs on
 * its own copy, built when it starts.  A failure beeps as well as saying why,
 * since the window that shows it may be hidden.
 */
#ifdef __INTELLISENSE__
#include <clib/alib_protos.h>
#include <clib/commodities_protos.h>
#include <clib/dos_protos.h>
#include <clib/exec_protos.h>
#include <clib/intuition_protos.h>
#else
#include <clib/alib_protos.h>
#include <proto/commodities.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#endif

#include <dos/dos.h>
#include <libraries/commodities.h>
#include <string.h>

#include <format.h>

#include "vcgrab.h"

struct Library *CxBase;

#define NAME_TAIL 12u /* of a name's room, kept for -0001.mkv */
#define MESSAGE_KEY(hot) ((LONG)(hot) + 1) /* a hot key's message */
#define MESSAGE_POPUP 100

struct cx
{
	struct waiter waiter;
	struct settings settings;
	const char *popkey;
	BYTE priority;
	CxObj *broker;
	CxObj *hot[HOT_COUNT];
	struct MsgPort *port;

	struct record_params params; /* the recording's copy */
	ULONG number;                /* the next take's, to try first */
	char name[SETTING_ROOM(to)]; /* the recording's file */
	struct source src;           /* the recording's screen */
	struct record *record;
	BOOL quit;
};

/**
 * name_next() - Find the first free name of a take: base-0001.suffix and on.
 * @out:    where it goes
 * @room:   the bytes there
 * @base:   the start of the name
 * @number: the number to try first; left after the one used
 * @suffix: ".mkv" or ".jpg"
 *
 * Return: @out.
 */
static const char *name_next(char *out, ULONG room, const char *base, ULONG *number, const char *suffix)
{
	ULONG length = umin((ULONG)strlen(base), room - NAME_TAIL);
	memcpy(out, base, length);
	for (;;)
	{
		_SNPrintf((STRPTR)out + length, NAME_TAIL, (CONST_STRPTR) "-%04lu%s", (*number)++, suffix);
		BPTR lock = Lock((CONST_STRPTR)out, ACCESS_READ);
		if (lock == 0)
			return out;
		UnLock(lock);
	}
}

/**
 * cx_start() - Start a recording of the screen in front, unless one runs.
 * @c: the commodity
 */
static void cx_start(struct cx *c)
{
	if (c->record != NULL)
		return;
	if (source_find(&c->src, NULL))
	{
		settings_params(&c->settings, &c->params, name_next(c->name, sizeof(c->name), c->settings.to, &c->number, ".mkv"));
		c->record = record_start(&c->waiter, &c->src, &c->params);
		if (c->record == NULL)
			source_release(&c->src);
	}
	if (c->record == NULL)
	{
		DisplayBeep(NULL);
		return;
	}
	window_status("Recording %s: changes hold from the next take", (const char *)FilePart((CONST_STRPTR)c->name));
	window_recording(TRUE);
}

/**
 * cx_finish() - Finish a recording that is over, and let go of its screen.
 * @c: the commodity
 */
static void cx_finish(struct cx *c)
{
	int rc = record_finish(c->record);
	c->record = NULL;
	source_release(&c->src);
	window_recording(FALSE);
	if (rc != RETURN_OK)
		DisplayBeep(NULL);
}

/**
 * cx_still() - Grab a still of the screen in front.
 * @c: the commodity
 */
static void cx_still(struct cx *c)
{
	char name[SETTING_ROOM(to)];
	const struct settings *s = &c->settings;
	if (still_take(s->width, s->height, NULL, name_next(name, sizeof(name), s->to, &c->number, ".jpg")) != RETURN_OK)
		DisplayBeep(NULL);
}

/**
 * key_make() - Make one hot key.
 * @c:      the commodity
 * @key:    its description
 * @id:     the message it sends
 *
 * Return: the object, not attached; NULL if @key describes nothing.
 */
static CxObj *key_make(struct cx *c, const char *key, LONG id)
{
	CxObj *hotkey = HotKey((CONST_STRPTR)key, c->port, id);
	if (hotkey != NULL && CxObjError(hotkey) != 0)
	{
		DeleteCxObjAll(hotkey);
		hotkey = NULL;
	}
	return hotkey;
}

/**
 * keys_set() - Replace the hot keys: all of them, or none.
 * @c: the commodity
 * @s: the settings with the new keys
 *
 * Return: TRUE, FALSE with the reason said and the old keys still on.
 */
static BOOL keys_set(struct cx *c, const struct settings *s)
{
	CxObj *fresh[HOT_COUNT];
	memset(fresh, 0, sizeof(fresh));
	for (ULONG i = 0; i < HOT_COUNT; i++)
	{
		if (s->key[i][0] == 0)
			continue;
		fresh[i] = key_make(c, s->key[i], MESSAGE_KEY(i));
		if (fresh[i] == NULL)
		{
			grab_say("\"%s\" is no hot key", s->key[i]);
			while (i-- != 0)
				if (fresh[i] != NULL)
					DeleteCxObjAll(fresh[i]);
			return FALSE;
		}
	}
	for (ULONG i = 0; i < HOT_COUNT; i++)
	{
		if (c->hot[i] != NULL)
			DeleteCxObjAll(c->hot[i]);
		c->hot[i] = fresh[i];
		if (fresh[i] != NULL)
			AttachCxObj(c->broker, fresh[i]);
	}
	return TRUE;
}

/**
 * cx_show() - Show the window; the status line tells of a recording under way.
 * @c: the commodity
 */
static void cx_show(struct cx *c)
{
	window_recording(c->record != NULL);
	if (!window_show(&c->settings))
		DisplayBeep(NULL);
}

/**
 * cx_apply() - Take what the window shows as the settings.
 * @c: the commodity
 *
 * Return: TRUE; FALSE with the window saying why not and nothing changed.
 */
static BOOL cx_apply(struct cx *c)
{
	struct settings s;
	const char *why = window_read(&s);
	if (why != NULL)
		grab_say("%s", why);
	if (why != NULL || !keys_set(c, &s))
	{
		DisplayBeep(NULL);
		return FALSE;
	}
	if (strcmp(s.to, c->settings.to) != 0)
		c->number = 1;
	c->settings = s;
	return TRUE;
}

/**
 * cx_window() - Act on what the window's buttons said.
 * @c: the commodity
 */
static void cx_window(struct cx *c)
{
	enum window_action action = window_handle();
	switch (action)
	{
	case WINDOW_SAVE:
	case WINDOW_USE:
		if (!cx_apply(c))
			break;
		if (settings_save(&c->settings, action == WINDOW_SAVE))
			window_hide();
		else
			DisplayBeep(NULL);
		break;
	case WINDOW_CANCEL:
		window_status("");
		window_hide();
		break;
	case WINDOW_RECORD:
		if (cx_apply(c))
			cx_start(c);
		break;
	case WINDOW_STOP:
		if (c->record != NULL)
			record_stop(c->record);
		break;
	case WINDOW_STILL:
		if (cx_apply(c))
			cx_still(c);
		break;
	default:
		break;
	}
}

/**
 * cx_command() - Act on a command from Exchange.
 * @c:  the commodity
 * @id: the CXCMD_*
 */
static void cx_command(struct cx *c, ULONG id)
{
	switch (id)
	{
	case CXCMD_KILL:
		c->quit = TRUE;
		break;
	case CXCMD_DISABLE:
	case CXCMD_ENABLE:
		ActivateCxObj(c->broker, id == CXCMD_ENABLE);
		break;
	case CXCMD_APPEAR:
	case CXCMD_UNIQUE:
		cx_show(c);
		break;
	case CXCMD_DISAPPEAR:
		window_hide();
		break;
	default:
		break;
	}
}

/**
 * cx_messages() - Act on the hot keys and Exchange's commands that have come.
 * @c: the commodity
 */
static void cx_messages(struct cx *c)
{
	CxMsg *m;
	while ((m = (CxMsg *)GetMsg(c->port)) != NULL)
	{
		ULONG id = (ULONG)CxMsgID(m), type = CxMsgType(m);
		ReplyMsg((struct Message *)m);
		if (type == CXM_COMMAND)
			cx_command(c, id);
		else if (id == MESSAGE_POPUP)
			cx_show(c);
		else if (id == MESSAGE_KEY(HOT_START))
			cx_start(c);
		else if (id == MESSAGE_KEY(HOT_STOP) && c->record != NULL)
			record_stop(c->record);
		else if (id == MESSAGE_KEY(HOT_STILL))
			cx_still(c);
	}
}

/**
 * cx_popkey() - Attach the key that shows the window; the default if the one asked for is none.
 * @c: the commodity
 */
static void cx_popkey(struct cx *c)
{
	CxObj *hotkey = key_make(c, c->popkey, MESSAGE_POPUP);
	if (hotkey == NULL)
		hotkey = key_make(c, POPKEY_DEFAULT, MESSAGE_POPUP);
	if (hotkey != NULL)
		AttachCxObj(c->broker, hotkey);
}

/**
 * cx_open() - Set the commodity up: the library, the port, the broker and its hot keys.
 * @c: the commodity
 *
 * A second start ends here, its NBU_NOTIFY having told the first to show
 * its window, which is what the user wanted.  Saved keys that make no sense
 * give way to the default ones, so the commodity still runs.
 *
 * Return: RETURN_OK with the broker active, else the return code with the
 * reason said, or RETURN_WARN for a second start; cx_close() either way.
 */
static int cx_open(struct cx *c)
{
	CxBase = OpenLibrary((CONST_STRPTR) "commodities.library", 37);
	c->port = CxBase != NULL ? CreateMsgPort() : NULL;
	if (c->port == NULL)
	{
		grab_say("commodities.library is missing");
		return RETURN_ERROR;
	}
	struct NewBroker nb;
	memset(&nb, 0, sizeof(nb));
	nb.nb_Version = NB_VERSION;
	nb.nb_Name = (STRPTR) "VCGrab";
	nb.nb_Title = (STRPTR) "VCGrab";
	nb.nb_Descr = (STRPTR) "Records the screen through the VideoCore";
	nb.nb_Unique = NBU_UNIQUE | NBU_NOTIFY;
	nb.nb_Flags = COF_SHOW_HIDE;
	nb.nb_Pri = c->priority;
	nb.nb_Port = c->port;
	LONG error = 0;
	c->broker = CxBroker(&nb, &error);
	if (c->broker == NULL)
	{
		if (error == CBERR_DUP)
			return RETURN_WARN;
		grab_say("the commodity did not start");
		return RETURN_ERROR;
	}
	if (!keys_set(c, &c->settings))
	{
		struct settings fresh;
		settings_default(&fresh);
		memcpy(c->settings.key, fresh.key, sizeof(fresh.key));
		(void)keys_set(c, &c->settings);
	}
	cx_popkey(c);
	ActivateCxObj(c->broker, TRUE);
	return RETURN_OK;
}

/**
 * cx_close() - Take the commodity down, as far as cx_open() got.
 * @c: the commodity
 */
static void cx_close(struct cx *c)
{
	if (c->broker != NULL)
		DeleteCxObjAll(c->broker);
	if (c->port != NULL)
	{
		struct Message *m;
		while ((m = GetMsg(c->port)) != NULL)
			ReplyMsg(m);
		DeleteMsgPort(c->port);
	}
	if (CxBase != NULL)
		CloseLibrary(CxBase);
}

/**
 * cx_run() - Serve the hot keys and the window until Kill or Ctrl-C; a recording under way then finishes its file.
 * @c: the commodity
 *
 * The window's signal is 0 while it is hidden, so the mask is made each turn.
 */
static void cx_run(struct cx *c)
{
	ULONG port = 1UL << c->port->mp_SigBit;
	while (!c->quit || c->record != NULL)
	{
		ULONG window = window_sigmask();
		ULONG seen = c->record != NULL ? record_pass(c->record, port | window) : Wait(port | window | SIGBREAKF_CTRL_C);
		if (seen & SIGBREAKF_CTRL_C)
			c->quit = TRUE;
		if (seen & port)
			cx_messages(c);
		if (seen & window)
			cx_window(c);
		if (c->record != NULL)
		{
			if (c->quit)
				record_stop(c->record);
			if (record_over(c->record))
				cx_finish(c);
		}
	}
}

/**
 * commodity() - Stay in the background as a commodity.
 * @args: how it starts
 *
 * Return: the return code.
 */
int commodity(const struct commodity_args *args)
{
	static struct cx c;
	c.settings = args->settings;
	c.popkey = args->popkey;
	c.priority = (BYTE)args->priority;
	c.number = 1;
	if (!waiter_open(&c.waiter))
		return RETURN_ERROR;
	int rc = cx_open(&c);
	if (rc == RETURN_OK)
	{
		if (args->popup)
			cx_show(&c);
		cx_run(&c);
	}
	window_end();
	cx_close(&c);
	waiter_close(&c.waiter);
	return rc == RETURN_WARN ? RETURN_OK : rc;
}
