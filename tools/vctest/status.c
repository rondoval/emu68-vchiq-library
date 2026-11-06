/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * status.c - STATUS: vchiq.device's connection and counters (NSCMD_VQ_GETINFO)
 * and every codec driver's (VC_GetBackendInfo()); CODECS: the codec table
 * (VC_QueryCodec()).  The same facts VCInfo prints for people, as key=value.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/videocodec_protos.h>
#else
#include <proto/exec.h>
#include <proto/videocodec.h>
#endif

#include <dos/dos.h>
#include <exec/errors.h>
#include <string.h>

#include <bits.h>
#include <kv.h>
#include <timing.h>
#include <vc_names.h>

#include "commands.h"

const char status_template[] = "";
const char codecs_template[] = "OF/M";

/* vi_Bringup's values, by number */
static const char *const bringups[] = {"fresh", "resumed", "repaired", "reinit"};

/**
 * do_status() - STATUS: the connection's state and counters, and every backend's.
 * @t:    the tool
 * @args: none
 *
 * Return: RETURN_OK; RETURN_ERROR when vchiq.device did not open; RETURN_FAIL when it did not answer.
 */
int do_status(struct tool *t, LONG *args)
{
	(void)args;
	struct vq_service *s = &t->svc;
	if (!vq_open(s, &t->waiter.timer, 0, 0, 0, NULL))
	{
		kv_num("vchiq.error", s->error);
		kv_str("fail", s->error == IOERR_OPENFAIL ? "vchiq.device did not load" : "no link to the VideoCore");
		return RETURN_ERROR;
	}
	struct Library *device = &s->io->iovq_Req.io_Device->dd_Library;
	kv_version("vchiq.device", device->lib_Version, device->lib_Revision);
	struct VchiqInfo info;
	BOOL ok = vq_get_info(s, &info);
	vq_close(s);
	if (!ok)
	{
		kv_str("fail", "NSCMD_VQ_GETINFO failed");
		return RETURN_FAIL;
	}
	kv_hex("vchiq.window", info.vi_WindowBase);
	kv_unum("vchiq.window_bytes", info.vi_WindowSize);
	kv_unum("vchiq.session", info.vi_Sessions);
	kv_unum("vchiq.version", info.vi_VersionCommon);
	kv_hex("vchiq.flags", info.vi_Flags);
	kv_unum("timer.now", get_time());
	kv_unum("vchiq.services", info.vi_Services);
	kv_unum("vchiq.rx_pos", info.vi_RxPos);
	kv_unum("vchiq.tx_pos", info.vi_TxPos);
	kv_unum("vchiq.msgs_tx", info.vi_MsgsTx);
	kv_unum("vchiq.msgs_rx", info.vi_MsgsRx);
	kv_unum("vchiq.slots_in", info.vi_SlotsIn);
	kv_unum("vchiq.slots_out", info.vi_SlotsOut);
	kv_unum("vchiq.bell_rings", info.vi_BellRings);
	kv_unum("vchiq.interrupts", info.vi_Interrupts);
	kv_unum("vchiq.bulks", info.vi_BulksDone);
	kv_unum("vchiq.unexpected", info.vi_Unexpected);
	kv_unum("vchiq.dropped", info.vi_Dropped);
	kv_unum("vchiq.stalls", info.vi_RxStalls);
	kv_str("vchiq.bringup", info.vi_Bringup < ARRAY_SIZE(bringups) ? bringups[info.vi_Bringup] : "?");
	kv_hex("vchiq.reach.start", info.vi_ReachStart);
	kv_hex("vchiq.reach.end", info.vi_ReachEnd);
	kv_hex("vchiq.reach.bus", info.vi_ReachBus);
	kv_hex("vchiq.uncached.start", info.vi_UncachedStart);
	kv_hex("vchiq.uncached.end", info.vi_UncachedEnd);

	ULONG n = 0;
	for (; VideoCodecBase != NULL; n++)
	{
		struct VCCounter counters[16];
		struct VCInfo vi;
		vi.Size = sizeof(vi);
		vi.Counters = counters;
		vi.MaxCounters = ARRAY_SIZE(counters);
		if (VC_GetBackendInfo(n, &vi) != 0)
			break;
		kv_str(kv_at("backend", n, "name"), (const char *)vi.Backend);
		kv_version(kv_at("backend", n, "abi"), vi.ABIVersion >> 16, vi.ABIVersion & 0xffffu);
		kv_unum(kv_at("backend", n, "codecs"), vi.Codecs);
		kv_unum(kv_at("backend", n, "sessions"), vi.Sessions);
		kv_str(kv_at("backend", n, "interface"), (const char *)vi.Interface);
		for (ULONG k = 0; k < vi.NumCounters && k < vi.MaxCounters; k++)
		{
			char field[8 + sizeof(counters[k].Name)] = "counter.";
			strncat(field, (const char *)counters[k].Name, sizeof(counters[k].Name) - 1u);
			kv_unum(kv_at("backend", n, field), counters[k].Value);
		}
		if (VC_HAS_FIELD(vi.Size, struct VCInfo, MemFree))
		{
			kv_unum(kv_at("backend", n, "mem_free"), vi.MemFree);
			kv_unum(kv_at("backend", n, "mem_total"), vi.MemTotal);
		}
	}
	kv_unum("backends", n);
	return RETURN_OK;
}

/**
 * format_list() - VCF_MASK()s as a comma-separated list of names.
 * @formats: the mask
 * @out:     room for the list
 * @room:    its bytes
 *
 * Return: @out.
 */
static const char *format_list(ULONG formats, char *out, ULONG room)
{
	ULONG at = 0;
	for (ULONG f = 0; f < 32; f++)
	{
		if (!(formats & VCF_MASK(f)))
			continue;
		const char *s = format_name(f);
		if (at != 0 && at < room - 1u)
			out[at++] = ',';
		while (*s != 0 && at < room - 1u)
			out[at++] = *s++;
	}
	out[at] = 0;
	return out;
}

/**
 * do_codecs() - CODECS: every row of the codec table; with OF, the codec of each FourCC given.
 * @t:    the tool
 * @args: OF, FourCCs (VC_CodecOfTag()), each printed as of.N.tag and of.N.codec
 *
 * Return: RETURN_OK; RETURN_FAIL when there is no row.
 */
int do_codecs(struct tool *t, LONG *args)
{
	(void)t;
	char tag[TAG_NAME_SIZE];
	ULONG n = 0;
	for (;; n++)
	{
		struct VCCodecInfo ci;
		ci.Size = sizeof(ci);
		if (VC_QueryCodec(n, &ci) != 0)
			break;
		char list[256]; /* every format's name: about 150 characters */
		kv_str(kv_at("codec", n, "name"), codec_name(ci.Codec));
		kv_str(kv_at("codec", n, "kind"), kind_name(ci.Kind));
		kv_str(kv_at("codec", n, "backend"), (const char *)ci.Backend);
		kv_unum(kv_at("codec", n, "max_width"), ci.MaxWidth);
		kv_unum(kv_at("codec", n, "max_height"), ci.MaxHeight);
		kv_unum(kv_at("codec", n, "level"), ci.MaxLevel);
		if (ci.OutputFormats != 0)
			kv_str(kv_at("codec", n, "output_formats"), format_list(ci.OutputFormats, list, sizeof(list)));
		if (VC_HAS_FIELD(ci.Size, struct VCCodecInfo, InputFormats) && ci.InputFormats != 0)
			kv_str(kv_at("codec", n, "input_formats"), format_list(ci.InputFormats, list, sizeof(list)));
		kv_bool(kv_at("codec", n, "session"), (ci.Flags & VCCF_Session) != 0);
		kv_bool(kv_at("codec", n, "image"), (ci.Flags & VCCF_Image) != 0);
		kv_num(kv_at("codec", n, "priority"), ci.Priority);
		if (VC_HAS_FIELD(ci.Size, struct VCCodecInfo, Tag) && ci.Tag != 0)
			kv_str(kv_at("codec", n, "tag"), tag_name(ci.Tag, tag));
	}
	kv_unum("codecs", n);
	const char *const *of = (const char *const *)args[0];
	for (ULONG i = 0; of != NULL && of[i] != NULL; i++)
	{
		ULONG fourcc = tag_of_name(of[i]);
		kv_str(kv_at("of", i, "tag"), of[i]);
		kv_str(kv_at("of", i, "codec"), fourcc != 0 ? codec_name(VC_CodecOfTag(fourcc)) : "?");
	}
	if (n == 0)
	{
		kv_str("fail", "no codecs");
		return RETURN_FAIL;
	}
	return RETURN_OK;
}
