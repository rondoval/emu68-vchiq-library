/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * window.c - the settings window, in ReAction.  It shows a struct settings,
 * gives one back and says which button was pressed; what that means is
 * commodity.c's.
 *
 * It is built when shown and disposed of when hidden, so Cancel has nothing
 * to undo and a hidden window holds nothing.  The classes open at the first
 * show, as a commodity that never shows its window need not load them, and
 * stay until window_end().
 */
#ifdef __INTELLISENSE__
#include <clib/alib_protos.h>
#include <clib/button_protos.h>
#include <clib/chooser_protos.h>
#include <clib/exec_protos.h>
#include <clib/getfile_protos.h>
#include <clib/integer_protos.h>
#include <clib/intuition_protos.h>
#include <clib/label_protos.h>
#include <clib/layout_protos.h>
#include <clib/string_protos.h>
#include <clib/window_protos.h>
#else
#include <clib/alib_protos.h>
#include <proto/button.h>
#include <proto/chooser.h>
#include <proto/exec.h>
#include <proto/getfile.h>
#include <proto/integer.h>
#include <proto/intuition.h>
#include <proto/label.h>
#include <proto/layout.h>
#include <proto/string.h>
#include <proto/window.h>
#endif

#include <classes/window.h>
#include <gadgets/button.h>
#include <gadgets/chooser.h>
#include <gadgets/getfile.h>
#include <gadgets/integer.h>
#include <gadgets/layout.h>
#include <gadgets/string.h>
#include <images/label.h>
#include <intuition/gadgetclass.h>
#include <intuition/intuition.h>
#include <reaction/reaction_macros.h>
#include <stdarg.h>
#include <string.h>

#include <format.h>

#include "vcgrab.h"

struct Library *WindowBase;
struct Library *LayoutBase;
struct Library *ButtonBase;
struct Library *StringBase;
struct Library *IntegerBase;
struct Library *ChooserBase;
struct Library *GetFileBase;
struct Library *LabelBase;

#define CLASS_VERSION 45 /* CHOOSER_LabelArray */
#define STATUS_LONGEST "grab-0001.mkv: 18001 pictures, 600020 ms, 0 ticks dropped, no end mark back" /* what the status line is wide enough for */
#define DIGITS_SHOWN 8u

static const struct
{
	struct Library **base;
	const char *name;
} classes[] = {
    {&WindowBase, "window.class"},           {&LayoutBase, "gadgets/layout.gadget"},
    {&ButtonBase, "gadgets/button.gadget"},  {&StringBase, "gadgets/string.gadget"},
    {&IntegerBase, "gadgets/integer.gadget"}, {&ChooserBase, "gadgets/chooser.gadget"},
    {&GetFileBase, "gadgets/getfile.gadget"}, {&LabelBase, "images/label.image"},
};
#define CLASSES (sizeof(classes) / sizeof(classes[0]))

enum
{
	GID_DRAWER,
	GID_NAME,
	GID_SIZE,
	GID_RATE,
	GID_BITRATE,
	GID_LIMIT,
	GID_START_KEY,
	GID_STOP_KEY,
	GID_STILL_KEY,
	GID_RECORD,
	GID_STOP,
	GID_STILL,
	GID_STATUS,
	GID_SAVE,
	GID_USE,
	GID_CANCEL,
	GID_COUNT
};

/* The sizes offered: shares of the largest the encoder takes */
static const struct
{
	UBYTE times, over;
} shares[] = {{1, 1}, {5, 6}, {3, 4}, {2, 3}, {8, 15}, {1, 2}, {4, 9}, {1, 3}, {1, 4}, {1, 6}};
#define SHARES (sizeof(shares) / sizeof(shares[0]))

static Object *window_object;
static struct Window *window;
static Object *gadget[GID_COUNT];
static BOOL recording;
static char status[200];

/* What the gadgets point at for as long as they live */
static char drawer_text[SETTING_ROOM(to)];
static struct
{
	ULONG width, height; /* 0: the screen's */
	char text[24];
} size_rows[SHARES + 2u]; /* the screen's size, the shares, and one that is none of these */
static STRPTR size_labels[SHARES + 3u];

/**
 * classes_open() - Open the classes the window needs.
 *
 * Return: TRUE, FALSE with the reason said.
 */
static BOOL classes_open(void)
{
	for (ULONG i = 0; i < CLASSES; i++)
	{
		if (*classes[i].base == NULL)
			*classes[i].base = OpenLibrary((CONST_STRPTR)classes[i].name, CLASS_VERSION);
		if (*classes[i].base == NULL)
		{
			grab_say("%s is missing", classes[i].name);
			return FALSE;
		}
	}
	return TRUE;
}

/**
 * label() - Make a gadget's label.
 * @text: its text
 *
 * Return: the image, which the layout then owns.
 */
static Object *label(const char *text)
{
	return NewObject(LABEL_GetClass(), NULL, LABEL_Text, (ULONG)text, TAG_END);
}

/**
 * string_make() - Make a string gadget.
 * @id:   its GID_*
 * @text: what it shows
 * @room: its characters at most, the end mark among them
 *
 * Return: the gadget, NULL if it failed.
 */
static Object *string_make(ULONG id, const char *text, ULONG room)
{
	return NewObject(STRING_GetClass(), NULL, GA_ID, id, GA_RelVerify, TRUE, GA_TabCycle, TRUE, STRINGA_TextVal, (ULONG)text,
	                 STRINGA_MaxChars, room, TAG_END);
}

/**
 * integer_make() - Make an integer gadget.
 * @id:    its GID_*
 * @value: what it shows
 * @most:  the largest it takes
 *
 * Return: the gadget, NULL if it failed.
 */
static Object *integer_make(ULONG id, ULONG value, ULONG most)
{
	return NewObject(INTEGER_GetClass(), NULL, GA_ID, id, GA_RelVerify, TRUE, GA_TabCycle, TRUE, INTEGER_Number, value, INTEGER_Minimum, 0,
	                 INTEGER_Maximum, most, INTEGER_MinVisible, DIGITS_SHOWN, TAG_END);
}

/**
 * button_make() - Make a button.
 * @id:   its GID_*
 * @text: its text
 *
 * Return: the gadget, NULL if it failed.
 */
static Object *button_make(ULONG id, const char *text)
{
	return NewObject(BUTTON_GetClass(), NULL, GA_ID, id, GA_RelVerify, TRUE, GA_Text, (ULONG)text, TAG_END);
}

/**
 * row_make() - Make a row of three buttons, even in size.
 * @first: the first's GID_*
 * @a:     its text
 * @b:     the next's
 * @c:     the last's
 *
 * Return: the layout, NULL if something failed.
 */
static Object *row_make(ULONG first, const char *a, const char *b, const char *c)
{
	gadget[first] = button_make(first, a);
	gadget[first + 1u] = button_make(first + 1u, b);
	gadget[first + 2u] = button_make(first + 2u, c);
	if (gadget[first] == NULL || gadget[first + 1u] == NULL || gadget[first + 2u] == NULL)
		return NULL;
	return NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_HORIZ, LAYOUT_EvenSize, TRUE, LAYOUT_AddChild,
	                 (ULONG)gadget[first], LAYOUT_AddChild, (ULONG)gadget[first + 1u], LAYOUT_AddChild, (ULONG)gadget[first + 2u], TAG_END);
}

/**
 * size_row() - Fill in one row of the size chooser's list.
 * @row:    the row
 * @width:  its width
 * @height: its height
 *
 * Return: the row after it.
 */
static ULONG size_row(ULONG row, ULONG width, ULONG height)
{
	size_rows[row].width = width;
	size_rows[row].height = height;
	_SNPrintf((STRPTR)size_rows[row].text, sizeof(size_rows[row].text), (CONST_STRPTR) "%lux%lu", width, height);
	size_labels[row] = (STRPTR)size_rows[row].text;
	return row + 1u;
}

/**
 * size_choice() - Fill in the size chooser's list, with the size asked for in it.
 * @s: the settings
 *
 * With no encoder to ask there is only the screen's size, and the one asked for.
 *
 * Return: the size's row.
 */
static ULONG size_choice(const struct settings *s)
{
	ULONG max_w, max_h;
	encoder_limits(&max_w, &max_h);
	ULONG rows = size_row(0, 0, 0);
	strlcpy(size_rows[0].text, "As the screen", sizeof(size_rows[0].text));
	if (max_w != 0)
		for (ULONG i = 0; i < SHARES; i++)
			rows = size_row(rows, (max_w * shares[i].times / shares[i].over) & ~1u, (max_h * shares[i].times / shares[i].over) & ~1u);

	ULONG chosen = 0;
	while (chosen < rows && (size_rows[chosen].width != s->width || size_rows[chosen].height != s->height))
		chosen++;
	if (chosen == rows)
		rows = size_row(rows, s->width, s->height);
	size_labels[rows] = NULL;
	return chosen;
}

/**
 * gadgets_make() - Make the fields, showing the settings.
 * @s: the settings
 *
 * Return: TRUE when all were made.
 */
static BOOL gadgets_make(const struct settings *s)
{
	char name[SETTING_ROOM(to)];
	settings_split(s, drawer_text, name);
	ULONG size = size_choice(s);
	gadget[GID_DRAWER] = NewObject(GETFILE_GetClass(), NULL, GA_ID, GID_DRAWER, GA_RelVerify, TRUE, GETFILE_TitleText,
	                               (ULONG) "The drawer for the takes", GETFILE_Drawer, (ULONG)drawer_text, GETFILE_DrawersOnly, TRUE,
	                               GETFILE_ReadOnly, TRUE, TAG_END);
	gadget[GID_NAME] = string_make(GID_NAME, name, sizeof(name));
	gadget[GID_SIZE] = NewObject(CHOOSER_GetClass(), NULL, GA_ID, GID_SIZE, GA_RelVerify, TRUE, CHOOSER_PopUp, TRUE, CHOOSER_LabelArray,
	                             (ULONG)size_labels, CHOOSER_Selected, size, TAG_END);
	gadget[GID_RATE] = integer_make(GID_RATE, s->fps, FPS_MAX);
	gadget[GID_BITRATE] = integer_make(GID_BITRATE, s->kbit, KBIT_MAX);
	gadget[GID_LIMIT] = integer_make(GID_LIMIT, s->seconds, 0x7FFFFFFFu);
	gadget[GID_START_KEY] = string_make(GID_START_KEY, s->key[HOT_START], sizeof(s->key[HOT_START]));
	gadget[GID_STOP_KEY] = string_make(GID_STOP_KEY, s->key[HOT_STOP], sizeof(s->key[HOT_STOP]));
	gadget[GID_STILL_KEY] = string_make(GID_STILL_KEY, s->key[HOT_STILL], sizeof(s->key[HOT_STILL]));
	gadget[GID_STATUS] = NewObject(BUTTON_GetClass(), NULL, GA_ID, GID_STATUS, GA_ReadOnly, TRUE, GA_Text, (ULONG)status,
	                               BUTTON_Justification, BCJ_LEFT, BUTTON_DomainString, (ULONG)STATUS_LONGEST, TAG_END);
	for (ULONG i = GID_DRAWER; i <= GID_STILL_KEY; i++)
		if (gadget[i] == NULL)
			return FALSE;
	return gadget[GID_STATUS] != NULL;
}

/**
 * gadgets_dispose() - Dispose of what a failed build left, no layout having taken it.
 */
static void gadgets_dispose(void)
{
	for (ULONG i = 0; i < GID_COUNT; i++)
		if (gadget[i] != NULL)
			DisposeObject(gadget[i]);
	memset(gadget, 0, sizeof(gadget));
}

/**
 * window_build() - Make the window object and all it holds.
 * @s: the settings it shows
 *
 * Return: TRUE, FALSE with the reason said and nothing held.
 */
static BOOL window_build(const struct settings *s)
{
	Object *takes = NULL, *choices = NULL, *root = NULL;
	if (gadgets_make(s))
	{
		takes = row_make(GID_RECORD, "Record", "Stop", "Still");
		choices = row_make(GID_SAVE, "Save", "Use", "Cancel");
	}
	if (takes != NULL && choices != NULL)
		root = NewObject(LAYOUT_GetClass(), NULL, LAYOUT_Orientation, LAYOUT_ORIENT_VERT, LAYOUT_SpaceOuter, TRUE, LAYOUT_DeferLayout, TRUE,
		                 LAYOUT_AddChild, (ULONG)gadget[GID_DRAWER], CHILD_Label, (ULONG)label("Drawer"),
		                 LAYOUT_AddChild, (ULONG)gadget[GID_NAME], CHILD_Label, (ULONG)label("Name"),
		                 LAYOUT_AddChild, (ULONG)gadget[GID_SIZE], CHILD_Label, (ULONG)label("Size"),
		                 LAYOUT_AddChild, (ULONG)gadget[GID_RATE], CHILD_Label, (ULONG)label("Pictures a second"),
		                 LAYOUT_AddChild, (ULONG)gadget[GID_BITRATE], CHILD_Label, (ULONG)label("Bit rate in kbit/s, 0 automatic"),
		                 LAYOUT_AddChild, (ULONG)gadget[GID_LIMIT], CHILD_Label, (ULONG)label("Time limit in seconds, 0 none"),
		                 LAYOUT_AddChild, (ULONG)gadget[GID_START_KEY], CHILD_Label, (ULONG)label("Start key"),
		                 LAYOUT_AddChild, (ULONG)gadget[GID_STOP_KEY], CHILD_Label, (ULONG)label("Stop key"),
		                 LAYOUT_AddChild, (ULONG)gadget[GID_STILL_KEY], CHILD_Label, (ULONG)label("Still key"),
		                 LAYOUT_AddChild, (ULONG)takes, CHILD_WeightedHeight, 0,
		                 LAYOUT_AddChild, (ULONG)gadget[GID_STATUS], CHILD_WeightedHeight, 0,
		                 LAYOUT_AddChild, (ULONG)choices, CHILD_WeightedHeight, 0,
		                 TAG_END);
	if (root != NULL)
		window_object = NewObject(WINDOW_GetClass(), NULL, WA_Title, (ULONG) "VCGrab", WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget,
		                          TRUE, WA_Activate, TRUE, WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW,
		                          WINDOW_Position, WPOS_CENTERSCREEN, WINDOW_ParentGroup, (ULONG)root, TAG_END);
	if (window_object != NULL)
		return TRUE;
	if (root != NULL)
		DisposeObject(root);
	else
	{
		/* the rows own their buttons */
		if (takes != NULL)
		{
			DisposeObject(takes);
			gadget[GID_RECORD] = gadget[GID_STOP] = gadget[GID_STILL] = NULL;
		}
		if (choices != NULL)
		{
			DisposeObject(choices);
			gadget[GID_SAVE] = gadget[GID_USE] = gadget[GID_CANCEL] = NULL;
		}
		gadgets_dispose();
	}
	memset(gadget, 0, sizeof(gadget));
	grab_say("the window did not build");
	return FALSE;
}

/**
 * buttons_set() - Allow Record or Stop, whichever makes sense now.
 */
static void buttons_set(void)
{
	if (window == NULL)
		return;
	SetGadgetAttrs((struct Gadget *)gadget[GID_RECORD], window, NULL, GA_Disabled, recording, TAG_DONE);
	SetGadgetAttrs((struct Gadget *)gadget[GID_STOP], window, NULL, GA_Disabled, !recording, TAG_DONE);
}

/**
 * window_show() - Show the window with these settings, or bring it to the front.
 * @s: the settings
 *
 * A window on a screen behind stays there during a recording: bringing that
 * screen forward would pause the take.
 *
 * Return: TRUE, FALSE with the reason said.
 */
BOOL window_show(const struct settings *s)
{
	if (window == NULL)
	{
		if (!classes_open() || !window_build(s))
			return FALSE;
		window = RA_OpenWindow(window_object);
		if (window == NULL)
		{
			window_hide();
			grab_say("the window did not open");
			return FALSE;
		}
		buttons_set();
	}
	WindowToFront(window);
	ActivateWindow(window);
	if (!recording)
		ScreenToFront(window->WScreen);
	return TRUE;
}

/**
 * window_hide() - Close the window and dispose of it, edits and all.
 */
void window_hide(void)
{
	if (window_object == NULL)
		return;
	if (window != NULL)
		RA_CloseWindow(window_object);
	DisposeObject(window_object); /* it owns the layout and all in it */
	window_object = NULL;
	window = NULL;
	memset(gadget, 0, sizeof(gadget));
}

/**
 * window_end() - Hide the window and close the classes.
 */
void window_end(void)
{
	window_hide();
	for (ULONG i = 0; i < CLASSES; i++)
		if (*classes[i].base != NULL)
			CloseLibrary(*classes[i].base);
}

/**
 * window_sigmask() - Give the signal to wait for.
 *
 * Return: the mask, 0 while hidden.
 */
ULONG window_sigmask(void)
{
	ULONG mask = 0;
	if (window != NULL)
		GetAttr(WINDOW_SigMask, window_object, &mask);
	return mask;
}

/**
 * window_status() - Set the status line, kept for when the window shows.
 * @format: the line, for _VSNPrintf()
 */
void window_status(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	_VSNPrintf((STRPTR)status, sizeof(status), (CONST_STRPTR)format, args);
	va_end(args);
	if (window != NULL)
		SetGadgetAttrs((struct Gadget *)gadget[GID_STATUS], window, NULL, GA_Text, (ULONG)status, TAG_DONE);
}

/**
 * window_recording() - Tell the window whether a recording runs.
 * @now: TRUE if one does
 */
void window_recording(BOOL now)
{
	recording = now;
	buttons_set();
}

/**
 * window_handle() - Take what the window has to say.
 *
 * All messages are taken before anything happens, as WM_HANDLEINPUT needs:
 * the caller may then dispose of the window.  The drawer's button is this
 * file's own: its requester blocks the task until it closes, which a
 * recording could not stand.
 *
 * Return: the last button that means something to the caller; nothing if
 * the window went since its signal came, as Exchange's Hide may do.
 */
enum window_action window_handle(void)
{
	enum window_action action = WINDOW_NOTHING;
	WORD code = 0;
	ULONG result;
	while (window != NULL && (result = RA_HandleInput(window_object, &code)) != WMHI_LASTMSG)
	{
		if ((result & WMHI_CLASSMASK) == WMHI_CLOSEWINDOW)
			action = WINDOW_CANCEL;
		else if ((result & WMHI_CLASSMASK) == WMHI_GADGETUP)
			switch (result & WMHI_GADGETMASK)
			{
			case GID_DRAWER:
				if (recording)
					window_status("Stop the recording to change the drawer: the requester would stall it");
				else
					(void)gfRequestDir(gadget[GID_DRAWER], window);
				break;
			case GID_RECORD:
				action = WINDOW_RECORD;
				break;
			case GID_STOP:
				action = WINDOW_STOP;
				break;
			case GID_STILL:
				action = WINDOW_STILL;
				break;
			case GID_SAVE:
				action = WINDOW_SAVE;
				break;
			case GID_USE:
				action = WINDOW_USE;
				break;
			case GID_CANCEL:
				action = WINDOW_CANCEL;
				break;
			default:
				break;
			}
	}
	return action;
}

/**
 * value_of() - Read a gadget's attribute.
 * @id:   its GID_*
 * @attr: the attribute
 *
 * Return: the value.
 */
static ULONG value_of(ULONG id, ULONG attr)
{
	ULONG value = 0;
	GetAttr(attr, gadget[id], &value);
	return value;
}

/**
 * window_read() - Give the settings the window shows.
 * @s: where they go
 *
 * Return: NULL, or why they will not do, in plain words.
 */
const char *window_read(struct settings *s)
{
	memset(s, 0, sizeof(*s));
	const char *why = settings_join(s, (const char *)value_of(GID_DRAWER, GETFILE_Drawer), (const char *)value_of(GID_NAME, STRINGA_TextVal));
	if (why != NULL)
		return why;
	ULONG size = value_of(GID_SIZE, CHOOSER_Selected);
	s->width = size_rows[size].width;
	s->height = size_rows[size].height;
	s->fps = value_of(GID_RATE, INTEGER_Number);
	if (s->fps == 0)
		return "The pictures a second must be 1 or more";
	s->kbit = value_of(GID_BITRATE, INTEGER_Number);
	s->seconds = value_of(GID_LIMIT, INTEGER_Number);
	(void)settings_key(s, HOT_START, (const char *)value_of(GID_START_KEY, STRINGA_TextVal)); /* their gadgets hold no more than fits */
	(void)settings_key(s, HOT_STOP, (const char *)value_of(GID_STOP_KEY, STRINGA_TextVal));
	(void)settings_key(s, HOT_STILL, (const char *)value_of(GID_STILL_KEY, STRINGA_TextVal));
	return settings_check(s);
}
