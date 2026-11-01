/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * auds_proto.h - the firmware's audio service ("AUDS") over VCHIQ: the
 * messages videocore.audio and VCTest AUDIO send and receive.
 *
 * The messages follow userland/interface/vmcs_host/vc_vchi_audioserv_defs.h
 * (Copyright (c) 2012, Broadcom Europe Ltd, BSD-3-Clause).  Every message is
 * one 20-byte struct auds_msg in either direction, little-endian: swap each
 * 32-bit field with VCHIQ_LE32() and each of WRITE's two 16-bit fields with
 * VCHIQ_LE16().
 *
 * The client sends OPEN, then CONTROL and CONFIG, then WRITEs and START.  The
 * VPU answers CONTROL, CONFIG and CLOSE with a RESULT (success 0); OPEN and
 * WRITE get none.  A WRITE with max_packet 0 announces count bytes of samples
 * that follow as one bulk transmit.  COMPLETE comes unasked as the VPU
 * consumes samples: its count is bytes consumed since the previous COMPLETE,
 * not one per WRITE.  STOP's draining 1 discards what is queued.
 */
#ifndef AUDS_PROTO_H
#define AUDS_PROTO_H

#include <exec/types.h>
#include <devices/vchiq.h>

/* The VCHIQ service, and the protocol versions a client offers */
#define AUDS_SERVICE_FOURCC VCHIQ_FOURCC('A', 'U', 'D', 'S')
#define AUDS_SERVICE_VERSION 2     /* LATENCY and CONFIG's channelmap are version 3: unused */
#define AUDS_SERVICE_VERSION_MIN 1

/* struct auds_msg.type */
enum auds_msg_type
{
	AUDS_MSG_RESULT = 0, /* VPU: the answer to CONTROL, CONFIG, CLOSE */
	AUDS_MSG_COMPLETE,   /* VPU: samples consumed */
	AUDS_MSG_CONFIG,
	AUDS_MSG_CONTROL,
	AUDS_MSG_OPEN,
	AUDS_MSG_CLOSE,
	AUDS_MSG_START,
	AUDS_MSG_STOP,
	AUDS_MSG_WRITE,
	AUDS_MSG_LATENCY, /* version 3 */
};

/* CONTROL's dest */
#define AUDS_DEST_AUTO 0
#define AUDS_DEST_HEADPHONES 1 /* the 3.5 mm jack (Pi 4, Pi 400; a CM4's carrier may have none) */
#define AUDS_DEST_HDMI0 2
#define AUDS_DEST_HDMI1 3

/* CONTROL's volume: attenuation in 1/256 dB, 0 unity, negative a gain */
#define AUDS_VOLUME_UNITY 0
#define AUDS_VOLUME_MUTE 26214 /* 102.4 dB down */

/* WRITE's cookies, echoed in COMPLETE */
#define AUDS_WRITE_COOKIE1 0x42434d41u /* 'BCMA' */
#define AUDS_WRITE_COOKIE2 0x44415441u /* 'DATA' */

/* COMPLETE's count: the bytes in the low 30 bits; bit 30 is set while the VPU ran out of samples */
#define AUDS_COMPLETE_BYTES 0x3fffffffu
#define AUDS_COMPLETE_UNDERRUN 0x40000000u

struct auds_msg
{
	ULONG type; /* enum auds_msg_type */
	union
	{
		struct
		{
			ULONG channels, samplerate, bps; /* bps 8 (unsigned) or 16 (signed) */
			ULONG channelmap;                /* version 3; 0 */
		} config;
		struct
		{
			ULONG volume, dest; /* AUDS_VOLUME_*, AUDS_DEST_* */
		} control;
		struct
		{
			ULONG dummy;
		} simple; /* OPEN, CLOSE, START, LATENCY */
		struct
		{
			ULONG draining;
		} stop;
		struct
		{
			ULONG count;            /* bytes */
			ULONG cookie1, cookie2; /* AUDS_WRITE_COOKIE1, AUDS_WRITE_COOKIE2 */
			UWORD silence;          /* 0: the samples follow */
			UWORD max_packet;       /* 0: as one bulk transmit */
		} write;
		struct
		{
			LONG success; /* 0 */
		} result;
		struct
		{
			ULONG count; /* AUDS_COMPLETE_BYTES, AUDS_COMPLETE_UNDERRUN */
			ULONG cookie1, cookie2;
		} complete;
	} u;
};

_Static_assert(sizeof(struct auds_msg) == 20, "AUDS message layout");

#endif /* AUDS_PROTO_H */
