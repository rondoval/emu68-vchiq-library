/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_gencmd.c - the VPU's codec memory, from the firmware's gencmd
 * service (videocore_gencmd.h has the why).
 *
 * gencmd answers text commands on a VCHIQ service of its own, a
 * little-endian status word and then text, sizes in megabytes:
 *   "get_mem reloc_total"  ->  0, "reloc_total=172M"
 *   "get_mem reloc"        ->  0, "reloc=167M"     (the free part)
 * An answer takes about a millisecond; a lost connection fails the read.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#endif

#include <memory.h>
#include <strutil.h>

#include <videocore_private.h>

#define GENCMD_FOURCC VCHIQ_FOURCC('G', 'C', 'M', 'D')
#define GENCMD_VERSION 1
#define GENCMD_ANSWER_MAX 64

/**
 * ask() - Send one command to an open gencmd unit and read its answer.
 * @SysBase: exec
 * @request: the open unit
 * @command: the command
 * @answer:  room for the answer
 *
 * Return: the answer's text, NUL-terminated inside @answer, or NULL if the
 * firmware did not answer with status 0.
 */
static const char *ask(struct ExecBase *SysBase, struct IOStdReq *request, const char *command, UBYTE answer[GENCMD_ANSWER_MAX])
{
	request->io_Command = CMD_WRITE;
	request->io_Data = (APTR)command;
	request->io_Length = strlen(command) + 1u;
	if (DoIO((struct IORequest *)request) != 0)
		return NULL;
	request->io_Command = CMD_READ;
	request->io_Data = answer;
	request->io_Length = GENCMD_ANSWER_MAX - 1u; /* room for the NUL */
	if (DoIO((struct IORequest *)request) != 0 || request->io_Actual < 4u || VCHIQ_LE32(*(const ULONG *)answer) != 0)
		return NULL;
	answer[request->io_Actual] = 0;
	return (const char *)answer + 4;
}

/**
 * answer_bytes() - The size a "name=<n>M" answer gives, converted to bytes.
 * @text: the answer, e.g. "reloc=167M" (gencmd counts in megabytes)
 *
 * Return: the size in bytes (167M gives 175112192), 0 if the answer has no
 * number, 0xffffffff from 4096M up.
 */
static ULONG answer_bytes(const char *text)
{
	while (*text != 0 && *text != '=')
		text++;
	if (*text == 0)
		return 0;
	ULONG value = 0;
	while (*++text >= '0' && *text <= '9' && value < 4096u)
		value = value * 10u + (ULONG)(*text - '0');
	return value < 4096u ? value << 20 : 0xffffffffu; /* 4 GiB and up do not fit */
}

/**
 * gencmd_codec_memory() - The VPU's codec memory: the relocatable heap's size and its free part.
 * @base:       the library base; no lock is taken or needed
 * @total:      where the heap's size in bytes goes
 * @free_bytes: where its free bytes go
 *
 * Opens a gencmd unit for the two questions and closes it again.
 *
 * Return: TRUE with both set, FALSE with both 0 if gencmd did not answer.
 */
BOOL gencmd_codec_memory(struct VideoCoreBase *base, ULONG *total, ULONG *free_bytes)
{
	struct ExecBase *SysBase = base->SysBase;
	*total = 0;
	*free_bytes = 0;
	struct MsgPort *port = CreateMsgPort();
	if (port == NULL)
		return FALSE;

	struct IOVchiqReq request;
	memset(&request, 0, sizeof(request));
	request.iovq_Req.io_Message.mn_ReplyPort = port;
	request.iovq_Req.io_Message.mn_Length = sizeof(request);
	request.iovq_FourCC = GENCMD_FOURCC;
	request.iovq_Version = GENCMD_VERSION;
	request.iovq_VersionMin = GENCMD_VERSION;
	if (OpenDevice((CONST_STRPTR)VCHIQ_NAME, 0, (struct IORequest *)&request, 0) == 0)
	{
		UBYTE total_answer[GENCMD_ANSWER_MAX], free_answer[GENCMD_ANSWER_MAX];
		const char *total_text = ask(SysBase, &request.iovq_Req, "get_mem reloc_total", total_answer);
		const char *free_text = total_text != NULL ? ask(SysBase, &request.iovq_Req, "get_mem reloc", free_answer) : NULL;
		if (free_text != NULL)
		{
			*total = answer_bytes(total_text);
			*free_bytes = *total != 0 ? answer_bytes(free_text) : 0;
		}
		CloseDevice((struct IORequest *)&request);
	}
	DeleteMsgPort(port);
	return *total != 0;
}
