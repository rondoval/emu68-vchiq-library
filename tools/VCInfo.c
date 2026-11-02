/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * VCInfo - whether the VideoCore is set up, and what it can do: the check to
 * run after installing, and what a bug report needs.
 *
 *   VCInfo                            the setup in plain words, each part OK
 *                                     or a warning with its cure (with
 *                                     vcjpeg.datatype installed, whether its
 *                                     descriptor comes first for JPEGs)
 *   VCInfo CODECS                     what the codec drivers do: codec, kind,
 *                                     limits, the pixel formats it writes
 *                                     and reads, and whether a
 *                                     session or an image call serves it
 *   VCInfo GCMD command               one gencmd command and its answer, as
 *                                     vcgencmd gives it
 *   VCInfo COMPONENTS                 which MMAL components the firmware has
 *                                     (each created and destroyed at once,
 *                                     never enabled), their ports' encodings,
 *                                     and gencmd's codec switches
 *   VCInfo COUNTERS                   the connection's state and counters, and
 *                                     every codec driver's
 *   VCInfo DUMP                       the shared window's state, read only
 *
 * One command per run; each ends with PASS or FAIL.
 *
 * Hosted CLI program (libnix).  A VCHIQ service is plain Exec I/O
 * (vchiq_client.c).
 *
 * Return codes: 0 PASS, 5 PASS with warnings, 10 a part is not installed,
 * 20 failed.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/datatypes_protos.h>
#include <clib/videocodec_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/datatypes.h>
#include <proto/videocodec.h>
#endif

#include <exec/types.h>
#include <exec/errors.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <dos/dos.h>
#include <dos/rdargs.h>

#include <stdlib.h>
#include <string.h>

#include <bits.h>
#include <devtree.h>
#include <drv_timer.h>
#include <strutil.h>
#include <timing.h>
#include <devices/vchiq.h>
#include <libraries/videocodec.h>
#include <vchiq/vchiq_core.h> /* the window layout, read-only, for DUMP */
#include <mmal.h>             /* MMAL's values and the wire format, for COMPONENTS */
#include <mmal_proto.h>

#include "vchiq_client.h"
#include "vc_names.h"

struct Library *VideoCodecBase;
struct Library *DataTypesBase;
APTR DeviceTreeBase;

static const char verstag[] __attribute__((used)) = VERSTAG;

static struct drv_timer timer;
static struct vq_service svc; /* big: the message buffers */

/* ---- the shared window ------------------------------------------------------- */

/**
 * dump_window() - The shared window's state, read straight from it (read-only: it is mapped and non-cacheable).
 * @info: the device's state, for the window's address
 * @when: what the dump is for, printed
 *
 * After a missing reply:
 *   VPU tx_pos != our rx_pos   the reply is in the ring; the device did not process it
 *   VPU trigger fired = 1      the VPU has not looked at the ring since we signalled it
 *   neither                    the VPU consumed the request and did not answer
 */
static void dump_window(const struct VchiqInfo *info, const char *when)
{
	volatile struct vchiq_host_state *host = (volatile struct vchiq_host_state *)info->vi_WindowBase;
	volatile struct vchiq_slot_zero *slot_zero =
	    (volatile struct vchiq_slot_zero *)(info->vi_WindowBase + host->slot_zero_offset);
	volatile struct vchiq_shared_state *m = &slot_zero->master;
	volatile struct vchiq_shared_state *s = &slot_zero->slave;

	Printf((CONST_STRPTR) "  dump (%s): device interrupts %ld, bell rings %ld, msgs tx %ld rx %ld\n", when, info->vi_Interrupts,
	    info->vi_BellRings, info->vi_MsgsTx, info->vi_MsgsRx);
	Printf((CONST_STRPTR) "    host: flags 0x%lx rx_pos %ld (slot %ld) tx_pos %ld (slot %ld) slots avail %ld\n", host->flags,
	    host->rx_pos, host->rx_slot, host->tx_pos, host->tx_slot, host->slot_queue_available);
	Printf((CONST_STRPTR) "    VPU:  tx_pos %ld, trigger a/f %ld/%ld, recycle a/f %ld/%ld, recycle idx %ld\n",
	    VCHIQ_LE32(m->tx_pos), VCHIQ_LE32(m->trigger.armed), VCHIQ_LE32(m->trigger.fired), VCHIQ_LE32(m->recycle.armed),
	    VCHIQ_LE32(m->recycle.fired), VCHIQ_LE32(m->slot_queue_recycle));
	Printf((CONST_STRPTR) "    ours: tx_pos %ld, trigger a/f %ld/%ld, recycle a/f %ld/%ld, recycle idx %ld\n",
	    VCHIQ_LE32(s->tx_pos), VCHIQ_LE32(s->trigger.armed), VCHIQ_LE32(s->trigger.fired), VCHIQ_LE32(s->recycle.armed),
	    VCHIQ_LE32(s->recycle.fired), VCHIQ_LE32(s->slot_queue_recycle));
	Printf((CONST_STRPTR) "    VPU debug[] %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld\n", VCHIQ_LE32(m->debug[0]),
	    VCHIQ_LE32(m->debug[1]), VCHIQ_LE32(m->debug[2]), VCHIQ_LE32(m->debug[3]), VCHIQ_LE32(m->debug[4]),
	    VCHIQ_LE32(m->debug[5]), VCHIQ_LE32(m->debug[6]), VCHIQ_LE32(m->debug[7]), VCHIQ_LE32(m->debug[8]),
	    VCHIQ_LE32(m->debug[9]), VCHIQ_LE32(m->debug[10]));
}

/**
 * do_dump() - DUMP: the shared window's state, through an information-only unit.
 *
 * Return: RETURN_OK, RETURN_FAIL if the device did not open or answer.
 */
static int do_dump(void)
{
	if (!vq_open(&svc, &timer, 0, 0, 0, "vchiq.device"))
		return RETURN_FAIL;
	struct VchiqInfo info;
	BOOL ok = vq_get_info(&svc, &info);
	vq_close(&svc);
	if (!ok)
	{
		Printf((CONST_STRPTR) "NSCMD_VQ_GETINFO failed\n");
		return RETURN_FAIL;
	}
	dump_window(&info, "now");
	return RETURN_OK;
}

/* ---- COUNTERS ------------------------------------------------------------------ */

/**
 * do_counters() - COUNTERS: the connection's state and counters, and every codec driver's.
 *
 * Return: RETURN_OK, RETURN_FAIL if the device did not open or answer.
 */
static int do_counters(void)
{
	if (!vq_open(&svc, &timer, 0, 0, 0, "vchiq.device"))
		return RETURN_FAIL;
	struct VchiqInfo info;
	BOOL ok = vq_get_info(&svc, &info);
	vq_close(&svc);
	if (!ok)
	{
		Printf((CONST_STRPTR) "NSCMD_VQ_GETINFO failed\n");
		return RETURN_FAIL;
	}
	Printf((CONST_STRPTR) "window 0x%08lx (%ld KiB), session %ld, VCHIQ version %ld, flags 0x%lx, %ld services open\n",
	    info.vi_WindowBase, info.vi_WindowSize / 1024u, info.vi_Sessions, info.vi_VersionCommon, info.vi_Flags, info.vi_Services);
	Printf((CONST_STRPTR) "positions: rx %ld, tx %ld; messages tx %ld rx %ld; slots in %ld out %ld; bell rings %ld, interrupts %ld; bulks %ld; unexpected %ld, dropped %ld, stalls %ld\n",
	    info.vi_RxPos, info.vi_TxPos, info.vi_MsgsTx, info.vi_MsgsRx, info.vi_SlotsIn, info.vi_SlotsOut, info.vi_BellRings,
	    info.vi_Interrupts, info.vi_BulksDone, info.vi_Unexpected, info.vi_Dropped, info.vi_RxStalls);
	Printf((CONST_STRPTR) "reach: 0x%08lx-0x%08lx at bus 0x%08lx; uncached 0x%08lx-0x%08lx\n", info.vi_ReachStart, info.vi_ReachEnd,
	    info.vi_ReachBus, info.vi_UncachedStart, info.vi_UncachedEnd);
	for (ULONG i = 0; VideoCodecBase != NULL; i++)
	{
		struct VCCounter counters[16];
		struct VCInfo vi;
		vi.Size = sizeof(vi);
		vi.Counters = counters;
		vi.MaxCounters = ARRAY_SIZE(counters);
		if (VC_GetBackendInfo(i, &vi) != 0)
			break;
		Printf((CONST_STRPTR) "%s: ABI %ld.%ld, %ld codecs, %ld sessions", vi.Backend, vi.ABIVersion >> 16,
		    vi.ABIVersion & 0xffffu, vi.Codecs, vi.Sessions);
		if (vi.Interface[0] != 0)
			Printf((CONST_STRPTR) ", %s", vi.Interface);
		for (ULONG k = 0; k < vi.NumCounters && k < vi.MaxCounters; k++)
			Printf((CONST_STRPTR) "%s%s %ld", k == 0 ? "; " : ", ", counters[k].Name, counters[k].Value);
		Printf((CONST_STRPTR) "\n");
		if (VC_HAS_FIELD(vi.Size, struct VCInfo, MemFree) && vi.MemTotal != 0)
			Printf((CONST_STRPTR) "%s: codec memory %ld MiB free of %ld MiB\n", vi.Backend, vi.MemFree >> 20,
			    vi.MemTotal >> 20);
	}
	return RETURN_OK;
}

/* ---- GCMD -------------------------------------------------------------------- */

/**
 * text_after() - What follows a mark in a text.
 * @text: the text
 * @mark: what comes before
 *
 * Return: the first character after the mark, NULL if the text does not have it.
 */
static char *text_after(char *text, const char *mark)
{
	char *at = strstr(text, mark);
	return at != NULL ? at + strlen(mark) : NULL;
}

/**
 * number_after() - The number that follows a mark in a text.
 * @text: the text
 * @mark: what comes before the number, with the answer's name: an answer of
 *        "error=1 error_msg=..." must not read as a value
 * @n:    where the number goes
 *
 * Return: TRUE if the mark is followed by a number, decimal or "0x" hex.
 */
static BOOL number_after(char *text, const char *mark, ULONG *n)
{
	char *digits = text_after(text, mark);
	if (digits == NULL)
		return FALSE;
	char *end;
	*n = strtoul(digits, &end, 0);
	return end != digits;
}

/**
 * cut_at() - End a text at the first of some characters.
 * @text: the text, cut in place
 * @ends: the characters
 *
 * Return: @text.
 */
static char *cut_at(char *text, const char *ends)
{
	text[strcspn(text, ends)] = 0;
	return text;
}

/**
 * trim_end() - Cut the spaces and newlines off a text's end.
 * @text: the text, cut in place
 */
static void trim_end(char *text)
{
	ULONG n = (ULONG)strlen(text);
	while (n > 0 && (text[n - 1u] == '\n' || text[n - 1u] == ' '))
		n--;
	text[n] = 0;
}

/**
 * gcmd_ask() - One gencmd command on the open GCMD service, and its answer.
 * @cmd:    the command
 * @status: where gencmd's status goes (0: done)
 * @text:   where the answer goes, NUL-terminated
 * @room:   its bytes
 * @took:   where the round trip's microseconds go, or NULL
 * @bytes:  where the reply's length goes, or NULL
 *
 * Return: TRUE if an answer came; FALSE on a write error (printed) or no reply.
 */
static BOOL gcmd_ask(const char *cmd, LONG *status, char *text, ULONG room, u32 *took, ULONG *bytes)
{
	u32 t0 = get_time();
	LONG q = vq_do(&svc, CMD_WRITE, cmd, (u32)strlen(cmd) + 1u, NULL);
	if (q != 0)
	{
		Printf((CONST_STRPTR) "  write \"%s\": %ld\n", cmd, q);
		return FALSE;
	}
	UBYTE *data;
	ULONG len;
	if (!vq_next(&svc, &data, &len, VQ_REPLY_TIMEOUT_MS))
		return FALSE;
	if (took != NULL)
		*took = get_time() - t0;
	if (bytes != NULL)
		*bytes = len;
	*status = len >= 4 ? (LONG)VCHIQ_LE32(*(ULONG *)data) : -1;
	ULONG n = len > 4 ? umin(len - 4u, room - 1u) : 0;
	CopyMem(data + 4, text, n);
	text[n] = 0;
	trim_end(text);
	vq_consume(&svc);
	return TRUE;
}

/**
 * do_gcmd() - gencmd commands through the GCMD service, each answer printed.
 * @cmds:  the commands, each sent in turn
 * @ncmds: how many
 *
 * A command left unanswered dumps the shared window, at once and 2 s later.
 *
 * Return: RETURN_OK, RETURN_FAIL if the service did not open, a command went
 * unanswered or one answered with an error.
 */
static int do_gcmd(const char *const *cmds, ULONG ncmds)
{
	if (!vq_open(&svc, &timer, GCMD_FOURCC, 1, 1, "GCMD"))
		return RETURN_FAIL;

	int rc = RETURN_OK;
	for (ULONG c = 0; c < ncmds; c++)
	{
		LONG status;
		char text[256];
		u32 took = 0;
		ULONG bytes = 0;
		if (!gcmd_ask(cmds[c], &status, text, sizeof(text), &took, &bytes))
		{
			Printf((CONST_STRPTR) "  \"%s\": no reply\n", cmds[c]);
			struct VchiqInfo info;
			if (vq_get_info(&svc, &info))
			{
				dump_window(&info, "at the timeout");
				Delay(100); /* 2 s: does anything still move? */
				if (vq_get_info(&svc, &info))
					dump_window(&info, "2 s later");
			}
			rc = RETURN_FAIL;
			break;
		}
		Printf((CONST_STRPTR) "  \"%s\" -> status %ld, %ld bytes, %ld us: %s\n", cmds[c], status, bytes, (ULONG)took, text);
		if (status != 0)
			rc = RETURN_FAIL;
	}

	vq_close(&svc);
	Printf((CONST_STRPTR) "close GCMD\n");
	return rc;
}

/* ---- COMPONENTS -------------------------------------------------------------- */

/* The components COMPONENTS tries: MMAL has no list of its own, so the names are guessed */
static const char *const probe_components[] = {
    "ril.video_decode", "ril.video_encode",   "ril.image_decode", "ril.image_encode", "ril.isp",          "ril.image_fx",
    "ril.resize",       "ril.video_splitter", "ril.video_render", "ril.null_sink",    "ril.source",       "ril.clock",
    "ril.camera",       "ril.camera_info",    "ril.rawcam",       "ril.hvs",          "ril.audio_render", "ril.audio_capture",
    "ril.audio_decode", "video_convert",      "aggregator.pipeline:ril.image_decode:video_convert",
};

/* The codecs COMPONENTS asks gencmd about: a name gencmd does not know just answers an error */
static const char *const codec_queries[] = {
    "codec_enabled H264", "codec_enabled MJPG", "codec_enabled MPG2", "codec_enabled WVC1", "codec_enabled MPG4",
    "codec_enabled VP6",  "codec_enabled VP8",  "codec_enabled THRA", "codec_enabled WMV9", "codec_enabled HEVC",
};

#define PROBE_ENCODINGS 48u /* encodings read per port */

/**
 * mmal_call() - One MMAL request on the open "mmal" service, and its reply.
 * @message: the request, its header filled here
 * @size:    its bytes
 * @msgid:   MMAL_MSG_*
 * @reply:   where the reply goes
 * @room:    its bytes
 *
 * A message that is not the reply (another id or context) is skipped.
 *
 * Return: the reply's bytes copied, 0 if none came.
 */
static ULONG mmal_call(void *message, ULONG size, ULONG msgid, void *reply, ULONG room)
{
	static ULONG context; /* each call's own, so a late reply to an earlier one is skipped */
	struct mmal_msg_header *header = message;
	header->magic = VCHIQ_LE32(MMAL_WIRE_MAGIC);
	header->msgid = VCHIQ_LE32(msgid);
	header->control_service = 0;
	header->context = VCHIQ_LE32(++context);
	header->status = 0;
	header->padding = 0;
	if (vq_do(&svc, CMD_WRITE, message, size, NULL) != 0)
		return 0;

	UBYTE *data;
	ULONG length;
	while (vq_next(&svc, &data, &length, VQ_REPLY_TIMEOUT_MS))
	{
		const struct mmal_msg_header *answer = (const struct mmal_msg_header *)data;
		BOOL mine = length >= sizeof(*answer) && VCHIQ_LE32(answer->msgid) == msgid && VCHIQ_LE32(answer->context) == context;
		ULONG copied = mine ? umin(length, room) : 0;
		if (copied != 0)
			CopyMem(data, reply, copied);
		vq_consume(&svc);
		if (mine)
			return copied;
	}
	return 0;
}

/**
 * fourcc_text() - A fourcc as its four characters, '.' for one that does not print.
 * @fourcc: the fourcc, MMAL's byte order (the first character lowest)
 * @text:   five bytes
 */
static void fourcc_text(ULONG fourcc, char text[5])
{
	for (ULONG i = 0; i < 4; i++)
	{
		char character = (char)(fourcc >> (8u * i));
		text[i] = character >= ' ' && character <= '~' ? character : '.';
	}
	text[4] = 0;
}

/**
 * probe_port() - Print a port: its format's encoding, its buffer needs, the encodings it takes.
 * @component: the component's handle, wire order
 * @type:      MMAL_PORT_TYPE_INPUT or MMAL_PORT_TYPE_OUTPUT
 * @index:     the port's index within its type
 */
static void probe_port(__le32 component, ULONG type, ULONG index)
{
	const char *side = type == MMAL_PORT_TYPE_INPUT ? "input" : "output";
	struct mmal_msg_port_info_get request = {0};
	struct mmal_msg_port_info info;
	request.component_handle = component;
	request.port_type = VCHIQ_LE32(type);
	request.index = VCHIQ_LE32(index);
	if (mmal_call(&request, sizeof(request), MMAL_MSG_PORT_INFO_GET, &info, sizeof(info)) < sizeof(info) || info.status != 0)
	{
		Printf((CONST_STRPTR) "  %s %ld: no port info\n", side, index);
		return;
	}
	char text[5];
	fourcc_text(VCHIQ_LE32(info.format.encoding), text);
	Printf((CONST_STRPTR) "  %s %ld: %s, buffers min %ld x %ld, recommended %ld x %ld\n", side, index, text,
	    VCHIQ_LE32(info.port.buffer_num_min), VCHIQ_LE32(info.port.buffer_size_min),
	    VCHIQ_LE32(info.port.buffer_num_recommended), VCHIQ_LE32(info.port.buffer_size_recommended));

	struct mmal_msg_port_param query = {0};
	struct mmal_msg_port_param_get_reply answer;
	/* the id and size words (8 bytes), then a fourcc (4 bytes) per encoding; the size says how much room the answer has */
	ULONG parameter = 8u + PROBE_ENCODINGS * 4u;
	query.component_handle = component;
	query.port_handle = info.port_handle;
	query.id = VCHIQ_LE32(MMAL_PARAMETER_SUPPORTED_ENCODINGS);
	query.size = VCHIQ_LE32(parameter);
	ULONG got = mmal_call(&query, MMAL_MSG_PORT_PARAM_FIXED + parameter, MMAL_MSG_PORT_PARAMETER_GET, &answer, sizeof(answer));
	/* at least the parameter's id and size words (8 bytes) */
	if (got < MMAL_MSG_PORT_PARAM_REPLY_FIXED + 8u || answer.status != 0)
	{
		Printf((CONST_STRPTR) "    encodings: none given\n");
		return;
	}
	/* the fourccs (4 bytes each) after the id and size words (8 bytes): as many as the size says, and as many as arrived */
	ULONG size = VCHIQ_LE32(answer.size);
	ULONG count = umin(size >= 8u ? (size - 8u) / 4u : 0, (got - MMAL_MSG_PORT_PARAM_REPLY_FIXED - 8u) / 4u);
	Printf((CONST_STRPTR) "    takes");
	for (ULONG i = 0; i < count; i++)
	{
		fourcc_text(VCHIQ_LE32(answer.space[i]), text);
		Printf((CONST_STRPTR) " %s", text);
	}
	Printf((CONST_STRPTR) "\n");
}

/**
 * do_components() - Which MMAL components the firmware has, their ports and encodings, and gencmd's codec switches.
 *
 * The firmware lists no components, so the names are tried.  Each one is
 * created, its input and output ports read, and destroyed at once: nothing
 * is enabled, no buffer moves.
 *
 * Return: RETURN_OK, RETURN_FAIL if the service did not open or a component
 * create went unanswered.  gencmd's answers are information only.
 */
static int do_components(void)
{
	if (!vq_open(&svc, &timer, MMAL_SERVICE_FOURCC, MMAL_SERVICE_VERSION, MMAL_SERVICE_VERSION_MIN, "mmal"))
		return RETURN_FAIL;

	struct mmal_msg_version version = {0};
	struct mmal_msg_version version_reply;
	if (mmal_call(&version, sizeof(version), MMAL_MSG_GET_VERSION, &version_reply, sizeof(version_reply)) == sizeof(version_reply))
		Printf((CONST_STRPTR) "  MMAL %ld.%ld (minimum %ld)\n", VCHIQ_LE32(version_reply.major), VCHIQ_LE32(version_reply.minor),
		    VCHIQ_LE32(version_reply.minimum));

	int rc = RETURN_OK;
	for (ULONG i = 0; i < ARRAY_SIZE(probe_components); i++)
	{
		const char *name = probe_components[i];
		struct mmal_msg_component_create create = {0};
		struct mmal_msg_component_create_reply created;
		create.client_component = VCHIQ_LE32(i + 1u); /* a cookie: nothing is enabled, so no event names it */
		strlcpy((char *)create.name, name, sizeof(create.name));
		if (mmal_call(&create, sizeof(create), MMAL_MSG_COMPONENT_CREATE, &created, sizeof(created)) < sizeof(created))
		{
			Printf((CONST_STRPTR) "%s: no reply\n", name);
			rc = RETURN_FAIL;
			break;
		}
		if (created.status != 0)
		{
			Printf((CONST_STRPTR) "%s: not created, status %ld\n", name, VCHIQ_LE32(created.status));
			continue;
		}
		ULONG inputs = VCHIQ_LE32(created.input_num), outputs = VCHIQ_LE32(created.output_num);
		Printf((CONST_STRPTR) "%s: %ld in, %ld out, %ld clock\n", name, inputs, outputs, VCHIQ_LE32(created.clock_num));
		for (ULONG k = 0; k < inputs && k < MMAL_WIRE_MAX_PORTS; k++)
			probe_port(created.component_handle, MMAL_PORT_TYPE_INPUT, k);
		for (ULONG k = 0; k < outputs && k < MMAL_WIRE_MAX_PORTS; k++)
			probe_port(created.component_handle, MMAL_PORT_TYPE_OUTPUT, k);

		struct mmal_msg_component_handle destroy = {0};
		struct mmal_msg_reply destroyed;
		destroy.component_handle = created.component_handle;
		if (mmal_call(&destroy, sizeof(destroy), MMAL_MSG_COMPONENT_DESTROY, &destroyed, sizeof(destroyed)) < sizeof(destroyed) ||
		    destroyed.status != 0)
			Printf((CONST_STRPTR) "  destroy: no reply or an error\n");
	}
	vq_close(&svc);
	Printf((CONST_STRPTR) "close mmal\n");

	(void)do_gcmd(codec_queries, ARRAY_SIZE(codec_queries));
	return rc;
}

/* ---- CODECS ------------------------------------------------------------------ */

/**
 * do_codecs() - CODECS: every codec driver's codec rows.
 *
 * Return: RETURN_OK, RETURN_FAIL if there are none.
 */
static int do_codecs(void)
{
	ULONG n = 0;
	for (ULONG i = 0;; i++)
	{
		struct VCCodecInfo ci;
		ci.Size = sizeof(ci);
		if (VC_QueryCodec(i, &ci) != 0)
			break;
		Printf((CONST_STRPTR) "  %s %s: up to %ldx%ld", codec_name(ci.Codec), kind_name(ci.Kind), ci.MaxWidth, ci.MaxHeight);
		if (ci.MaxLevel != 0)
			Printf((CONST_STRPTR) ", level %ld.%ld", ci.MaxLevel / 10u, ci.MaxLevel % 10u);
		if (ci.OutputFormats != 0)
		{
			Printf((CONST_STRPTR) "; writes");
			for (ULONG f = 0; f < 32; f++)
				if (ci.OutputFormats & VCF_MASK(f))
					Printf((CONST_STRPTR) " %s", format_name(f));
		}
		if (VC_HAS_FIELD(ci.Size, struct VCCodecInfo, InputFormats) && ci.InputFormats != 0)
		{
			Printf((CONST_STRPTR) "; reads");
			for (ULONG f = 0; f < 32; f++)
				if (ci.InputFormats & VCF_MASK(f))
					Printf((CONST_STRPTR) " %s", format_name(f));
		}
		Printf((CONST_STRPTR) "; %s%s%s, priority %ld\n", ci.Backend, (ci.Flags & VCCF_Session) ? ", session" : "",
		    (ci.Flags & VCCF_Image) ? ", image call" : "", ci.Priority);
		n++;
	}
	Printf((CONST_STRPTR) "%ld codecs\n", n);
	return n != 0 ? RETURN_OK : RETURN_FAIL;
}

/* ---- the summary --------------------------------------------------------------- */

/* The summary's state: the worst outcome so far, and the warnings. */
struct summary
{
	int rc;         /* RETURN_OK, RETURN_WARN, RETURN_ERROR (not installed) or RETURN_FAIL */
	ULONG warnings;
};

/**
 * worse() - Note an outcome; the summary keeps the worst.
 * @sum: the summary
 * @rc:  RETURN_WARN, RETURN_ERROR or RETURN_FAIL
 */
static void worse(struct summary *sum, int rc)
{
	if (rc == RETURN_WARN)
		sum->warnings++;
	if (rc > sum->rc)
		sum->rc = rc;
}

/* get_throttled's bits and their meanings, as the vcgencmd documentation words them */
static const struct
{
	ULONG mask;
	const char *meaning;
} throttle_bits[] = {
    {0x1, "Undervoltage detected"},
    {0x2, "Arm frequency capped"},
    {0x4, "Currently throttled"},
    {0x8, "Soft temperature limit active"},
    {0x10000, "Undervoltage has occurred"},
    {0x20000, "Arm frequency capping has occurred"},
    {0x40000, "Throttling has occurred"},
    {0x80000, "Soft temperature limit has occurred"},
};

/**
 * print_throttle() - Print what get_throttled's word says, or "none".
 * @value: the word
 */
static void print_throttle(ULONG value)
{
	const char *sep = "";
	for (ULONG i = 0; i < ARRAY_SIZE(throttle_bits); i++)
		if (value & throttle_bits[i].mask)
		{
			Printf((CONST_STRPTR) "%s%s", sep, throttle_bits[i].meaning);
			sep = ", ";
		}
	if (*sep == 0)
		Printf((CONST_STRPTR) "none");
}

/**
 * summary_emu68() - The parts Emu68 provides, read without vchiq.device: the resources and the VCHIQ node.
 * @sum: the summary
 *
 * Return: TRUE if vchiq.device can start.
 */
static BOOL summary_emu68(struct summary *sum)
{
	DeviceTreeBase = OpenResource((CONST_STRPTR) "devicetree.resource");
	APTR mailbox = OpenResource((CONST_STRPTR) "mailbox.resource");
	if (DeviceTreeBase == NULL || mailbox == NULL)
	{
		Printf((CONST_STRPTR) "  Emu68:          devicetree.resource or mailbox.resource missing\n");
		Printf((CONST_STRPTR) "  FAIL: not running under Emu68 1.1 or later\n");
		worse(sum, RETURN_FAIL);
		return FALSE;
	}
	Printf((CONST_STRPTR) "  Emu68:          device tree and mailbox present\n");

	APTR node = DT_OpenSymbol(DeviceTreeBase, (CONST_STRPTR) "vchiq"); /* no alias names it */
	if (node == NULL)
	{
		Printf((CONST_STRPTR) "  VCHIQ node:     none (no \"vchiq\" label in the Pi's device tree)\n");
		Printf((CONST_STRPTR) "  FAIL: this Emu68 or Pi firmware gives no VCHIQ node\n");
		worse(sum, RETURN_FAIL);
		return FALSE;
	}
	CONST_STRPTR compatible = DT_GetPropValue(DT_FindProperty(node, (CONST_STRPTR) "compatible"));
	Printf((CONST_STRPTR) "  VCHIQ node:     %s (%s)\n", (const char *)DT_GetKeyName(node),
	    compatible != NULL ? (const char *)compatible : "?");
	DT_CloseKey(node);

	if (OpenResource((CONST_STRPTR) "cma.resource") == NULL)
	{
		Printf((CONST_STRPTR) "  CMA:            cma.resource missing\n");
		Printf((CONST_STRPTR) "  FAIL: add dtoverlay=cma to the Pi's config.txt, then press Ctrl-Amiga-Amiga\n");
		worse(sum, RETURN_FAIL);
		return FALSE;
	}
	Printf((CONST_STRPTR) "  CMA:            cma.resource present\n");
	return TRUE;
}

/**
 * summary_device() - vchiq.device: whether it opens, and how its connection came up at this boot.
 * @sum: the summary
 *
 * Return: TRUE if the connection is up.
 */
static BOOL summary_device(struct summary *sum)
{
	if (!vq_open(&svc, &timer, 0, 0, 0, NULL))
	{
		if (svc.error == IOERR_OPENFAIL)
		{
			Printf((CONST_STRPTR) "  vchiq.device:   did not load\n");
			Printf((CONST_STRPTR) "  FAIL: copy vchiq.device to DEVS: (the Install script's videocodec)\n");
			worse(sum, RETURN_ERROR);
		}
		else
		{
			Printf((CONST_STRPTR) "  vchiq.device:   no link to the VideoCore (error %ld)\n", svc.error);
			Printf((CONST_STRPTR) "  FAIL: press Ctrl-Amiga-Amiga, and if that does not help power off and on; README, Troubleshooting\n");
			worse(sum, RETURN_FAIL);
		}
		return FALSE;
	}
	struct Library *device = &svc.io->iovq_Req.io_Device->dd_Library;
	struct VchiqInfo info;
	BOOL ok = vq_get_info(&svc, &info);
	Printf((CONST_STRPTR) "  vchiq.device:   %ld.%ld, connected", (ULONG)device->lib_Version, (ULONG)device->lib_Revision);
	vq_close(&svc);
	if (ok)
	{
		static const char *const bringups[] = {"a fresh start", "the previous session resumed", "the previous session repaired",
		                                       "started afresh: the VideoCore had restarted"};
		Printf((CONST_STRPTR) ", session %ld (%s)", info.vi_Sessions,
		    info.vi_Bringup < ARRAY_SIZE(bringups) ? bringups[info.vi_Bringup] : "?");
	}
	Printf((CONST_STRPTR) "\n");
	return TRUE;
}

/**
 * summary_firmware() - The VideoCore firmware through gencmd: version, temperature, throttling, clocks, gpu_mem.
 * @sum: the summary
 */
static void summary_firmware(struct summary *sum)
{
	if (!vq_open(&svc, &timer, GCMD_FOURCC, 1, 1, NULL))
	{
		Printf((CONST_STRPTR) "  firmware:       the gencmd service did not open (error %ld)\n", svc.error);
		Printf((CONST_STRPTR) "  FAIL: press Ctrl-Amiga-Amiga, and if that does not help power off and on; README, Troubleshooting\n");
		worse(sum, RETURN_FAIL);
		return;
	}
	LONG status;
	char text[256];
	if (!gcmd_ask("version", &status, text, sizeof(text), NULL, NULL) || status != 0)
	{
		Printf((CONST_STRPTR) "  firmware:       no answer from gencmd\n");
		Printf((CONST_STRPTR) "  FAIL: press Ctrl-Amiga-Amiga, and if that does not help power off and on; README, Troubleshooting\n");
		worse(sum, RETURN_FAIL);
		vq_close(&svc);
		return;
	}
	/* "<date>\nCopyright ...\nversion <hash> (...)": the date and the hash */
	char *hash = text_after(text, "\nversion ");
	if (hash != NULL)
		cut_at(hash, " \n");
	trim_end(cut_at(text, "\n"));
	Printf((CONST_STRPTR) "  firmware:       %s%s%s\n", text, hash != NULL ? ", version " : "", hash != NULL ? hash : "");

	ULONG value;
	if (gcmd_ask("measure_temp", &status, text, sizeof(text), NULL, NULL) && status == 0)
	{
		/* "temp=48.2'C" */
		char *temperature = text_after(text, "temp=");
		if (temperature != NULL)
			Printf((CONST_STRPTR) "  temperature:    %s C\n", cut_at(temperature, "'"));
	}

	if (gcmd_ask("get_throttled", &status, text, sizeof(text), NULL, NULL) && status == 0 && number_after(text, "throttled=", &value))
	{
		Printf((CONST_STRPTR) "  throttling:     ");
		print_throttle(value);
		Printf((CONST_STRPTR) "\n");
		ULONG seen = (value | (value >> 16)) & 0xfu; /* now or since the Pi started */
		if (seen & 1u)
		{
			Printf((CONST_STRPTR) "  WARN: the Pi's supply voltage drops: use a stronger power supply\n");
			worse(sum, RETURN_WARN);
		}
		if (seen & 8u)
		{
			Printf((CONST_STRPTR) "  WARN: the Pi reached its soft temperature limit: give it better cooling\n");
			worse(sum, RETURN_WARN);
		}
		if ((seen & 6u) != 0 && (seen & 9u) == 0) /* capped or throttled, and the word gives no cause */
		{
			Printf((CONST_STRPTR) "  WARN: the Pi's clocks were held back: check its cooling and power supply\n");
			worse(sum, RETURN_WARN);
		}
	}

	ULONG arm = 0, core = 0;
	/* "frequency(48)=1500345728" */
	if (gcmd_ask("measure_clock arm", &status, text, sizeof(text), NULL, NULL) && status == 0)
		(void)number_after(text, ")=", &arm);
	if (gcmd_ask("measure_clock core", &status, text, sizeof(text), NULL, NULL) && status == 0)
		(void)number_after(text, ")=", &core);
	if (arm != 0 || core != 0)
		Printf((CONST_STRPTR) "  clocks:         ARM %ld MHz, core %ld MHz\n", (arm + 500000u) / 1000000u,
		    (core + 500000u) / 1000000u);

	if (gcmd_ask("get_mem gpu", &status, text, sizeof(text), NULL, NULL) && status == 0 && number_after(text, "gpu=", &value))
		Printf((CONST_STRPTR) "  gpu_mem:        %ld MiB\n", value);
	vq_close(&svc);
}

/**
 * summary_library() - videocodec.library and its codec drivers.
 * @sum: the summary
 */
static void summary_library(struct summary *sum)
{
	if (VideoCodecBase == NULL)
	{
		/* no failure: sound needs only vchiq.device, and the Install script can leave the library out */
		Printf((CONST_STRPTR) "  videocodec:     not installed: video and pictures need %s\n", VIDEOCODEC_NAME);
		return;
	}
	Printf((CONST_STRPTR) "  videocodec:     %s %ld.%ld\n", VIDEOCODEC_NAME, (ULONG)VideoCodecBase->lib_Version,
	    (ULONG)VideoCodecBase->lib_Revision);
	ULONG backends = 0;
	for (ULONG i = 0;; i++)
	{
		struct VCInfo vi;
		vi.Size = sizeof(vi);
		vi.Counters = NULL; /* no counters here */
		if (VC_GetBackendInfo(i, &vi) != 0)
			break;
		Printf((CONST_STRPTR) "  codec driver:   %s, %ld codecs", vi.Backend, vi.Codecs);
		if (VC_HAS_FIELD(vi.Size, struct VCInfo, MemFree) && vi.MemTotal != 0)
			Printf((CONST_STRPTR) ", codec memory %ld MiB free of %ld MiB", vi.MemFree >> 20, vi.MemTotal >> 20);
		Printf((CONST_STRPTR) "\n");
		if (vi.Codecs == 0)
		{
			Printf((CONST_STRPTR) "  FAIL: %s offers no codecs: update it, or remove it from LIBS:videocodec/\n", vi.Backend);
			worse(sum, RETURN_FAIL);
		}
		backends++;
	}
	if (backends == 0)
	{
		Printf((CONST_STRPTR) "  codec driver:   none\n");
		Printf((CONST_STRPTR) "  FAIL: copy videocore.codec to LIBS:videocodec/ (the Install script's videocodec)\n");
		worse(sum, RETURN_ERROR);
	}
}

#define VCJPEG_CLASS "LIBS:datatypes/vcjpeg.datatype" /* how datatypes.library opens it: LIBS: takes in SYS:Classes */
#define VCJPEG_DESCRIPTOR "SYS:Storage/DataTypes/VCJPEG"
#define VCJPEG_PROBE "T:VCInfo-probe.jpg"

/* A JPEG's start and frame header and nothing else: baseline, 2000x1000, three components */
static const UBYTE probe_jpeg[] = {0xff, 0xd8, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x03, 0xe8, 0x07, 0xd0, 0x03,
                                   0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xd9};

/**
 * probe_descriptor() - The descriptor the system picks for a large baseline JPEG: a file of its header alone in T:.
 * @name: filled with the descriptor's name
 * @room: bytes at @name
 *
 * A file, not bytes in memory: some JPEG descriptors' functions examine the
 * file's lock.  Only the header is read; the file is deleted at once.
 *
 * Return: TRUE with @name filled, FALSE if the file could not be written or nothing took it.
 */
static BOOL probe_descriptor(char *name, ULONG room)
{
	BPTR f = Open((CONST_STRPTR)VCJPEG_PROBE, MODE_NEWFILE);
	if (f == 0)
		return FALSE;
	LONG put = Write(f, (APTR)probe_jpeg, sizeof(probe_jpeg));
	Close(f);
	BOOL found = FALSE;
	BPTR lock = put == (LONG)sizeof(probe_jpeg) ? Lock((CONST_STRPTR)VCJPEG_PROBE, ACCESS_READ) : 0;
	if (lock != 0)
	{
		struct DataType *dtn = ObtainDataType(DTST_FILE, (APTR)lock, TAG_DONE);
		if (dtn != NULL)
		{
			strlcpy(name, (const char *)dtn->dtn_Header->dth_Name, room);
			ReleaseDataType(dtn);
			found = TRUE;
		}
		UnLock(lock);
	}
	DeleteFile((CONST_STRPTR)VCJPEG_PROBE);
	return found;
}

/**
 * summary_datatype() - vcjpeg.datatype, when installed: whether its descriptor comes first for JPEGs.
 * @sum: the summary
 */
static void summary_datatype(struct summary *sum)
{
	BPTR cls = Lock((CONST_STRPTR)VCJPEG_CLASS, ACCESS_READ);
	if (cls == 0)
		return; /* not installed: it is optional */
	UnLock(cls);
	char name[40];
	if (DataTypesBase == NULL || !probe_descriptor(name, sizeof(name)))
	{
		Printf((CONST_STRPTR) "  JPEG datatype:  vcjpeg.datatype installed; which descriptor takes JPEGs could not be checked (T:)\n");
		return;
	}
	if (strcmp(name, "VCJPEG") == 0)
	{
		Printf((CONST_STRPTR) "  JPEG datatype:  vcjpeg.datatype takes the large JPEGs\n");
		return;
	}
	Printf((CONST_STRPTR) "  JPEG datatype:  large JPEGs go to %s, not vcjpeg.datatype\n", name);
	Printf((CONST_STRPTR) "  WARN: add the line AddDataTypes %s QUIET to S:User-Startup and reboot\n", VCJPEG_DESCRIPTOR);
	worse(sum, RETURN_WARN);
}

/**
 * do_summary() - The setup in plain words: each part OK, or a warning or failure with its cure.
 *
 * Return: RETURN_OK, RETURN_WARN, RETURN_ERROR (a part not installed) or RETURN_FAIL.
 */
static int do_summary(void)
{
	struct summary sum = {RETURN_OK, 0};
	Printf((CONST_STRPTR) "VideoCore setup\n");
	if (summary_emu68(&sum) && summary_device(&sum))
		summary_firmware(&sum);
	summary_library(&sum);
	summary_datatype(&sum);
	if (sum.rc == RETURN_OK)
		Printf((CONST_STRPTR) "PASS\n");
	else if (sum.rc == RETURN_WARN)
		Printf((CONST_STRPTR) "PASS, %ld warning%s\n", sum.warnings, sum.warnings == 1 ? "" : "s");
	else
		Printf((CONST_STRPTR) "FAIL\n");
	return sum.rc;
}

/* ---- main -------------------------------------------------------------------- */

enum
{
	ARG_CODECS,
	ARG_GCMD,
	ARG_COMPONENTS,
	ARG_COUNTERS,
	ARG_DUMP,
	ARG_COUNT
};

int main(void)
{
	LONG args[ARG_COUNT] = {0};
	struct RDArgs *rda = ReadArgs((CONST_STRPTR) "CODECS/S,GCMD/K,COMPONENTS/S,COUNTERS/S,DUMP/S", args, NULL);
	if (rda == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "VCInfo");
		return RETURN_FAIL;
	}

	ULONG commands = 0;
	for (ULONG i = 0; i < ARG_COUNT; i++)
		if (args[i] != 0)
			commands++;
	if (commands > 1)
	{
		Printf((CONST_STRPTR) "one of CODECS, GCMD, COMPONENTS, COUNTERS or DUMP at a time\n");
		FreeArgs(rda);
		return RETURN_FAIL;
	}
	if (!drv_timer_open(&timer, SysBase))
	{
		Printf((CONST_STRPTR) "no timer\n");
		FreeArgs(rda);
		return RETURN_FAIL;
	}
	VideoCodecBase = OpenLibrary((CONST_STRPTR)VIDEOCODEC_NAME, 0);
	DataTypesBase = commands == 0 ? OpenLibrary((CONST_STRPTR) "datatypes.library", 39) : NULL; /* the summary's datatype check */

	int rc;
	if (commands == 0)
		rc = do_summary();
	else
	{
		if (args[ARG_CODECS] != 0 && VideoCodecBase == NULL)
		{
			Printf((CONST_STRPTR) "%s did not open\n", VIDEOCODEC_NAME);
			rc = RETURN_ERROR;
		}
		else if (args[ARG_CODECS] != 0)
			rc = do_codecs();
		else if (args[ARG_GCMD] != 0)
		{
			const char *cmd = (const char *)args[ARG_GCMD];
			rc = do_gcmd(&cmd, 1);
		}
		else if (args[ARG_COMPONENTS] != 0)
			rc = do_components();
		else if (args[ARG_COUNTERS] != 0)
			rc = do_counters();
		else
			rc = do_dump();
		Printf((CONST_STRPTR) "%s\n", rc == RETURN_OK ? "PASS" : "FAIL");
	}

	if (DataTypesBase != NULL)
		CloseLibrary(DataTypesBase);
	if (VideoCodecBase != NULL)
		CloseLibrary(VideoCodecBase);
	drv_timer_close(&timer);
	FreeArgs(rda);
	return rc;
}
