/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_backend.c - the videocodec backend ABI (VCB_*): the library's
 * functions.  Each session call is a look-up of the session and one call
 * into the session layer under session_lock.
 */
#include <format.h>
#include <memory.h>
#include <strutil.h>

#include <videocore_private.h>

/**
 * add_counter() - One counter into the caller's list, if it has room, and counted.
 * @info:  the structure being filled, Counters and MaxCounters the caller's
 * @name:  the counter's name
 * @value: its value
 */
static void add_counter(struct VCInfo *info, const char *name, ULONG value)
{
	if (info->NumCounters < info->MaxCounters)
	{
		struct VCCounter *c = &info->Counters[info->NumCounters];
		strlcpy((char *)c->Name, name, sizeof(c->Name));
		c->Value = value;
	}
	info->NumCounters++;
}

/**
 * VCB_GetInfo() - What the backend is and how it is doing.
 * @info: the caller's structure (A0), filled as far as its Size reaches
 * @base: the library base (A6)
 *
 * The session count is one word, read without session_lock, which a session
 * call holds across its waits for the VPU.  The interface ("MMAL 0.2" while
 * the service is open) and the counters come through mmal_info(): events
 * (messages the VPU sent), events_dropped (lost for lack of room: should stay
 * 0), buffers_back (buffers the VPU returned) and stray (replies nobody waited
 * for: should stay 0).  The codec memory comes from gencmd, 0 and 0 if it
 * does not answer.
 *
 * Return: 0, or VCERR_BADPARAM for a structure too small for the ABI version.
 */
LONG VCB_GetInfo(struct VCInfo *info asm("a0"), struct VideoCoreBase *base asm("a6"))
{
	if (info == NULL || !VC_HAS_FIELD(info->Size, struct VCInfo, ABIVersion))
		return VCERR_BADPARAM;

	struct VCInfo all;
	memset(&all, 0, sizeof(all));
	all.Size = umin(info->Size, sizeof(all));
	if (VC_HAS_FIELD(all.Size, struct VCInfo, MaxCounters) && info->Counters != NULL)
	{
		all.Counters = info->Counters;
		all.MaxCounters = info->MaxCounters;
	}
	all.ABIVersion = VCB_ABI_VERSION;
	all.Codecs = codec_count();
	strlcpy((char *)all.Backend, LIBRARY_NAME, sizeof(all.Backend));
	all.Sessions = base->session_count;
	struct mmal_counters counters;
	mmal_info(base, &counters);
	if (counters.version_major != 0 || counters.version_minor != 0)
		_SNPrintf(all.Interface, sizeof(all.Interface), (CONST_STRPTR) "MMAL %u.%u", counters.version_major,
		    counters.version_minor);
	add_counter(&all, "events", counters.events);
	add_counter(&all, "events_dropped", counters.events_dropped);
	add_counter(&all, "buffers_back", counters.buffers_back);
	add_counter(&all, "stray", counters.stray_messages);
	(void)gencmd_codec_memory(base, &all.MemTotal, &all.MemFree);

	memcpy(info, &all, all.Size);
	return 0;
}

/**
 * VCB_QueryCodec() - One row of the backend's codec table.
 * @index: the row (D0), 0 upward
 * @info:  the caller's structure (A0), filled as far as its Size reaches
 * @base:  the library base (A6)
 *
 * Return: 0, or VCERR_BADPARAM past the last row or for a structure too small.
 */
LONG VCB_QueryCodec(ULONG index asm("d0"), struct VCCodecInfo *info asm("a0"), struct VideoCoreBase *base asm("a6"))
{
	(void)base;
	if (info == NULL || !VC_HAS_FIELD(info->Size, struct VCCodecInfo, Kind))
		return VCERR_BADPARAM;
	return codec_query(index, info);
}

/**
 * VCB_OpenSession() - Open a decoder, encoder or converter session.
 * @params: the frontend's parameters (A0)
 * @error:  where the error goes (A1), 0 on success; may be NULL
 * @base:   the library base (A6)
 *
 * Return: the session's handle, NULL with *@error set (VCERR_UNSUPPORTED for
 * a still picture's codec: the image calls' only, VCB_Image()).
 */
APTR VCB_OpenSession(struct VCSessionParams *params asm("a0"), LONG *error asm("a1"), struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	LONG result = VCERR_BADPARAM;
	struct vc_session *session = NULL;
	if (params != NULL && VC_HAS_FIELD(params->Size, struct VCSessionParams, ExtradataSize))
	{
		ObtainSemaphore(&base->session_lock);
		session = session_open(base, params, FALSE, &result);
		ReleaseSemaphore(&base->session_lock);
	}
	if (error != NULL)
		*error = result;
	return session;
}

/**
 * VCB_CloseSession() - Close a session; an unknown handle is ignored.
 * @handle: the session (A0)
 * @base:   the library base (A6)
 */
VOID VCB_CloseSession(APTR handle asm("a0"), struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->session_lock);
	struct vc_session *session = session_lookup(base, handle);
	if (session != NULL)
		session_close(session, FALSE);
	ReleaseSemaphore(&base->session_lock);
}

/**
 * VCB_ImportPacket() - A packet over the caller's memory: a decoder's input or an encoder's output.
 * @handle:   the session (A0)
 * @side:     VCS_Input or VCS_Output (D0)
 * @data:     the caller's memory (A1)
 * @capacity: its bytes (D1)
 * @base:     the library base (A6)
 *
 * Return: the packet, NULL if refused.
 */
struct VCPacket *VCB_ImportPacket(APTR handle asm("a0"), ULONG side asm("d0"), APTR data asm("a1"), ULONG capacity asm("d1"),
                                  struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->session_lock);
	struct vc_session *session = session_lookup(base, handle);
	struct VCPacket *packet = session != NULL ? session_import_packet(session, side, data, capacity) : NULL;
	ReleaseSemaphore(&base->session_lock);
	return packet;
}

/**
 * VCB_ImportFrame() - A frame over the caller's memory, as large as the side's frames.
 * @handle: the session (A0)
 * @side:   VCS_Input or VCS_Output (D0)
 * @data:   the caller's memory (A1)
 * @base:   the library base (A6)
 *
 * Return: the frame, described, or NULL if refused.
 */
struct VCFrame *VCB_ImportFrame(APTR handle asm("a0"), ULONG side asm("d0"), APTR data asm("a1"), struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->session_lock);
	struct vc_session *session = session_lookup(base, handle);
	struct VCFrame *frame = session != NULL ? session_import_frame(session, side, data) : NULL;
	ReleaseSemaphore(&base->session_lock);
	return frame;
}

/**
 * VCB_SendPacket() - Send a packet to a decoder.
 * @handle: the session (A0)
 * @packet: one of its input packets, in the caller's hands (A1)
 * @base:   the library base (A6)
 *
 * Return: 0 or the error.
 */
LONG VCB_SendPacket(APTR handle asm("a0"), struct VCPacket *packet asm("a1"), struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (packet == NULL)
		return VCERR_BADPARAM;
	ObtainSemaphore(&base->session_lock);
	struct vc_session *session = session_lookup(base, handle);
	LONG error = session != NULL ? session_send_packet(session, packet) : VCERR_BADPARAM;
	ReleaseSemaphore(&base->session_lock);
	return error;
}

/**
 * VCB_SendFrame() - Send a picture to an encoder or converter.
 * @handle: the session (A0)
 * @frame:  one of its input frames, in the caller's hands (A1)
 * @base:   the library base (A6)
 *
 * Return: 0 or the error.
 */
LONG VCB_SendFrame(APTR handle asm("a0"), struct VCFrame *frame asm("a1"), struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (frame == NULL)
		return VCERR_BADPARAM;
	ObtainSemaphore(&base->session_lock);
	struct vc_session *session = session_lookup(base, handle);
	LONG error = session != NULL ? session_send_frame(session, frame) : VCERR_BADPARAM;
	ReleaseSemaphore(&base->session_lock);
	return error;
}

/**
 * VCB_Poll() - The session's next item for the caller.
 * @handle: the session (A0)
 * @item:   where the item goes (A1); VCIT_None when there is none
 * @base:   the library base (A6)
 *
 * Return: 1 with @item filled, 0 if nothing is ready.
 */
LONG VCB_Poll(APTR handle asm("a0"), struct VCItem *item asm("a1"), struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (item == NULL)
		return 0;
	ObtainSemaphore(&base->session_lock);
	struct vc_session *session = session_lookup(base, handle);
	BOOL ready = session != NULL && session_poll(session, item);
	ReleaseSemaphore(&base->session_lock);
	if (!ready)
		item->Type = VCIT_None;
	return ready ? 1 : 0;
}

/**
 * VCB_ReleaseFrame() - The caller is done with an output frame.
 * @handle: the session (A0)
 * @frame:  the frame (A1)
 * @base:   the library base (A6)
 */
VOID VCB_ReleaseFrame(APTR handle asm("a0"), struct VCFrame *frame asm("a1"), struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (frame == NULL)
		return;
	ObtainSemaphore(&base->session_lock);
	struct vc_session *session = session_lookup(base, handle);
	if (session != NULL)
		session_release_frame(session, frame);
	ReleaseSemaphore(&base->session_lock);
}

/**
 * VCB_ReleasePacket() - The caller is done with an output packet.
 * @handle: the session (A0)
 * @packet: the packet (A1)
 * @base:   the library base (A6)
 */
VOID VCB_ReleasePacket(APTR handle asm("a0"), struct VCPacket *packet asm("a1"), struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (packet == NULL)
		return;
	ObtainSemaphore(&base->session_lock);
	struct vc_session *session = session_lookup(base, handle);
	if (session != NULL)
		session_release_packet(session, packet);
	ReleaseSemaphore(&base->session_lock);
}

/**
 * VCB_Flush() - Have the VPU return everything it holds.
 * @handle: the session (A0)
 * @base:   the library base (A6)
 *
 * Return: 0 or the error.
 */
LONG VCB_Flush(APTR handle asm("a0"), struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->session_lock);
	struct vc_session *session = session_lookup(base, handle);
	LONG error = session != NULL ? session_flush(session) : VCERR_BADPARAM;
	ReleaseSemaphore(&base->session_lock);
	return error;
}

/**
 * VCB_SetAttrs() - Change a running session.
 * @handle: the session (A0)
 * @params: the parameters, Changed naming which (A1)
 * @base:   the library base (A6)
 *
 * Return: 0 or the error.
 */
LONG VCB_SetAttrs(APTR handle asm("a0"), struct VCSessionParams *params asm("a1"), struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (params == NULL || !VC_HAS_FIELD(params->Size, struct VCSessionParams, ExtradataSize))
		return VCERR_BADPARAM;
	ObtainSemaphore(&base->session_lock);
	struct vc_session *session = session_lookup(base, handle);
	LONG error = session != NULL ? session_set_attrs(session, params) : VCERR_BADPARAM;
	ReleaseSemaphore(&base->session_lock);
	return error;
}

/**
 * VCB_Idle() - The frontend's last opener closed it: let go of what is kept between calls.
 * @base: the library base (A6)
 *
 * The MMAL service, kept open past the last component so that the next
 * image call or session finds it, closes; the next one opens it again.  The
 * closing task may wait for the VPU here.
 */
VOID VCB_Idle(struct VideoCoreBase *base asm("a6"))
{
	mmal_idle(base);
}
