/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * VCTest - the test rig of vchiq.device, videocodec.library, videocore.audio
 * and vcjpeg.datatype, driven by the test suite (tests/, developer-guide.md
 * section 8).  One command per run; its facts come out as key=value lines
 * (kv.h), the verdict last.  VCInfo shows the same state for people.
 *
 *   VCTest ?                           the commands
 *   VCTest <command> ?                 a command's template
 *
 *   STATUS                             vchiq.device's connection and counters, every backend's
 *   CODECS                             the codec table
 *   MARK text                          a mark in the debug log (the capture self-test)
 *   ABANDON                            leave the connection as a crash would (the repair test;
 *                                      needs a vchiq.device built with VCHIQ_TEST_HOOKS)
 *   AUDIO [MS n] [DEST n] [RATE n] [CHANNELS n] [CHUNK ms] [DEPTH n] [TONE] [WALK] [VOL n] [RESTART]
 *                                      the firmware audio service driven directly
 *   AHIWALK MODE n [RATE n]            the channel walk through an AHI mode
 *   AHITONE MODE n [MS n] [RATE n] [HZ n]
 *                                      the tone through an AHI mode, start and stop timed
 *   DECODE file [REF framemd5] [OUT file] [FRAMES n] [NOHASH] [ENCODING H264|MJPEG] [VERBOSE]
 *          [FRAMED] [FPS n] [OUTFORMAT f] [FLUSH n] [BACKEND name] [EXTRADATA] [PAD n] [NOPTS] [NORATE] [PACKET n]
 *                                      an elementary stream through a decoder session
 *   IMAGE file OUT pic [OUTWIDTH n] [OUTHEIGHT n] [OUTFORMAT f] [ENCODING c] [BACKEND name] [MISALIGN]
 *                                      one still through VC_DecodeImage()
 *   TOJPEG raw WIDTH n HEIGHT n OUT jpg [FORMAT f] [QUALITY n] [BACKEND name] [MISALIGN]
 *                                      one raw picture through VC_EncodeImage()
 *   CONVERT raw WIDTH n HEIGHT n OUT pic [FORMAT f] [OUTWIDTH n] [OUTHEIGHT n] [OUTFORMAT f]
 *           [BACKEND name] [MISALIGN]  one raw picture through VC_ConvertImage()
 *   ENCODE raw WIDTH n HEIGHT n OUT h264 [FPS n] [BITRATE n] [KEYINT n] [KEYREQ n] [FORMAT f] [OUTWIDTH n] [OUTHEIGHT n] [HOLD n] [GAPS] [ENDEMPTY] [BACKEND name]
 *                                      a raw file through an H.264 encoder session
 *   DATATYPE file [OUT pic]            a picture through datatypes.library
 *   PAGE file... [COPIES n]            many at once, a process each, as a browser loads a page
 *   SCREEN WIDTH n HEIGHT n [DEPTH n] [SECONDS n] [NOISE] [BACK n]
 *                                      a Picasso96 screen to record: colour bars, a block moving, or noise
 *
 * Return codes: 0 the command did its work and its checks passed (PASS),
 * 5 a check failed, 10 the environment (a library, device, file or memory
 * missing, no test hooks), 20 the API refused or failed, or the arguments
 * made no sense (then before the format line).
 *
 * Hosted CLI program (libnix).  A VCHIQ service is plain Exec I/O
 * (vchiq_client.c); a session signals this task and its items come through
 * VC_Poll().
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#endif

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/rdargs.h>
#include <string.h>

#include <debugport.h>
#include <kv.h>
#include <strutil.h>

#include "commands.h"

struct Library *VideoCodecBase;

/* libnix swaps to a stack of this size: the library's calls and waits run on it */
unsigned long __stack = 32768;

static const char verstag[] __attribute__((used)) = VERSTAG;

/* A command: its name, its ReadArgs() template, its entry point, whether it needs videocodec.library */
struct command
{
	const char *name;
	const char *template;
	command_fn run;
	BOOL needs_codec;
};

static int do_mark(struct tool *t, LONG *args);

static const struct command commands[] = {
    {"STATUS", status_template, do_status, FALSE},     {"CODECS", codecs_template, do_codecs, TRUE},
    {"MARK", "TEXT/F", do_mark, FALSE},                {"ABANDON", abandon_template, do_abandon, FALSE},
    {"AUDIO", audio_template, do_audio, FALSE},        {"AHIWALK", ahiwalk_template, do_ahiwalk, FALSE},
    {"AHITONE", ahitone_template, do_ahitone, FALSE},  {"DECODE", decode_template, do_decode, TRUE},
    {"IMAGE", image_template, do_image, TRUE},         {"TOJPEG", tojpeg_template, do_tojpeg, TRUE},
    {"CONVERT", convert_template, do_convert, TRUE},   {"ENCODE", encode_template, do_encode, TRUE},
    {"DATATYPE", datatype_template, do_datatype, FALSE}, {"PAGE", page_template, do_page, TRUE},
    {"SCREEN", screen_template, do_screen, FALSE},
    {"HOLD", hold_template, do_hold, FALSE},
};

/**
 * do_mark() - MARK: a line in the debug log, and the same text as mark=.
 * @t:    the tool
 * @args: TEXT/F
 *
 * Return: RETURN_OK.
 */
static int do_mark(struct tool *t, LONG *args)
{
	(void)t;
	const char *text = args[0] != 0 ? (const char *)args[0] : "";
	kv_str("mark", text);
	debugport_printf("[VCTest] mark %s\n", text);
	return RETURN_OK;
}

/**
 * command_named() - The command a name picks, any case.
 * @name: the name
 *
 * Return: the command, NULL for none.
 */
static const struct command *command_named(const char *name)
{
	for (ULONG i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
		if (_Stricmp((CONST_STRPTR)name, (CONST_STRPTR)commands[i].name) == 0)
			return &commands[i];
	return NULL;
}

/**
 * parse_rest() - Read a command's arguments from the rest of the line.
 * @template: the command's template
 * @rest:     the line after the command's name, or NULL
 * @args:     the array to fill, zeroed, ARGS_MAX entries
 *
 * A template of more entries than @args holds is refused: ReadArgs() would
 * write past it.
 *
 * Return: the RDArgs to free with FreeArgs() and FreeDosObject(); NULL on a usage error (IoErr() says).
 */
static struct RDArgs *parse_rest(const char *template, const char *rest, LONG *args)
{
	ULONG entries = 1;
	for (const char *p = template; *p != 0; p++)
		entries += *p == ',';
	if (entries > ARGS_MAX)
	{
		SetIoErr(ERROR_LINE_TOO_LONG); /* ReadArgs() would write past args[] */
		return NULL;
	}

	static char line[512];
	ULONG n = 0;
	for (const char *p = rest != NULL ? rest : ""; *p != 0 && n < sizeof(line) - 2u; p++)
		line[n++] = *p;
	line[n++] = '\n';
	line[n] = 0;
	struct RDArgs *rda = AllocDosObject(DOS_RDARGS, NULL);
	if (rda == NULL)
		return NULL;
	rda->RDA_Source.CS_Buffer = (STRPTR)line;
	rda->RDA_Source.CS_Length = (LONG)n;
	rda->RDA_Source.CS_CurChr = 0;
	rda->RDA_Flags |= RDAF_NOPROMPT;
	if (ReadArgs((CONST_STRPTR)template, args, rda) == NULL)
	{
		FreeDosObject(DOS_RDARGS, rda);
		return NULL;
	}
	return rda;
}

int main(void)
{
	LONG top[2] = {0, 0};
	struct RDArgs *rda = ReadArgs((CONST_STRPTR) "COMMAND/A,ARGS/F", top, NULL);
	if (rda == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "VCTest");
		return RETURN_FAIL;
	}
	const char *name = (const char *)top[0], *rest = (const char *)top[1];
	int rc = RETURN_FAIL;
	if (strcmp(name, "?") == 0 || _Stricmp((CONST_STRPTR)name, (CONST_STRPTR) "HELP") == 0)
	{
		for (ULONG i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
			Printf((CONST_STRPTR) "%s %s\n", commands[i].name, commands[i].template);
		rc = RETURN_OK;
		goto out_top;
	}
	const struct command *cmd = command_named(name);
	if (cmd == NULL)
	{
		Printf((CONST_STRPTR) "VCTest: no command %s (VCTest ? lists them)\n", name);
		goto out_top;
	}
	if (rest != NULL && strcmp(rest, "?") == 0)
	{
		Printf((CONST_STRPTR) "%s %s\n", cmd->name, cmd->template);
		rc = RETURN_OK;
		goto out_top;
	}
	LONG args[ARGS_MAX];
	for (ULONG i = 0; i < ARGS_MAX; i++)
		args[i] = 0;
	struct RDArgs *rda_cmd = NULL;
	if (cmd->template[0] != 0)
	{
		rda_cmd = parse_rest(cmd->template, rest, args);
		if (rda_cmd == NULL)
		{
			PrintFault(IoErr(), (CONST_STRPTR) "VCTest");
			goto out_top;
		}
	}

	kv_init("VCTest", FALSE);
	kv_str("command", cmd->name);
	kv_num("cli", ((struct Process *)FindTask(NULL))->pr_TaskNum);
	kv_begin((const char *)GetArgStr());
	rc = RETURN_ERROR;
	VideoCodecBase = OpenLibrary((CONST_STRPTR)VIDEOCODEC_NAME, 0);
	if (VideoCodecBase != NULL)
		kv_version("videocodec.version", VideoCodecBase->lib_Version, VideoCodecBase->lib_Revision);
	else if (cmd->needs_codec)
	{
		kv_str("fail", "videocodec.library did not open");
		goto out_run;
	}
	static struct tool t; /* big: the service's message buffers */
	memset(&t, 0, sizeof(t));
	t.waiter.sig = -1;
	if (!waiter_open(&t.waiter))
	{
		kv_str("fail", "no signal or timer");
		goto out_run;
	}
	rc = cmd->run(&t, args);
	waiter_close(&t.waiter);
out_run:
	kv_verdict(rc);
	kv_end(rc);
	if (VideoCodecBase != NULL)
		CloseLibrary(VideoCodecBase);
	if (rda_cmd != NULL)
	{
		FreeArgs(rda_cmd);
		FreeDosObject(DOS_RDARGS, rda_cmd);
	}
out_top:
	FreeArgs(rda);
	return rc;
}
