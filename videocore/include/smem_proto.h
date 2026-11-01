/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * smem_proto.h - the VPU's shared-memory service over VCHIQ ("SMEM").
 *
 * Written from the protocol as the Raspberry Pi kernel's vc-sm-cma driver
 * speaks it (GPL, a behavioural reference only: these are the protocol's
 * facts in our own definitions).  Every field is a little-endian 32-bit word.
 *
 * The host sends a header (type, transaction id) and a body.  The VPU
 * answers an IMPORT with the transaction id and the import's handle; FREE
 * and CLIENT_VERSION have no answer.  Messages the VPU sends unasked carry
 * bit 31 in their first word with their type below it: the CLIENT_VERSION
 * acknowledgement, RELEASED when its last reference to an import went, and
 * VC_MEM_REQUEST when it wants memory of the host.
 */
#ifndef SMEM_PROTO_H
#define SMEM_PROTO_H

#include <types.h>

/* The VCHIQ service and the versions the client offers */
#define SMEM_SERVICE_FOURCC VCHIQ_FOURCC('S', 'M', 'E', 'M')
#define SMEM_SERVICE_VERSION 1
#define SMEM_SERVICE_VERSION_MIN 0

/*
 * The client protocol version told to the VPU: 2 has it report RELEASED for
 * every import it lets go, and lets it ask for memory (refused here).
 */
#define SMEM_CLIENT_VERSION 2

/* Message types (the host's in the header, the VPU's in the first word with SMEM_FROM_VPU) */
enum smem_type
{
	SMEM_MSG_FREE = 4,                 /* host: let go of an import; no answer */
	SMEM_MSG_IMPORT = 8,               /* host: wrap memory of ours in a handle; answered */
	SMEM_MSG_CLIENT_VERSION = 9,       /* host: the protocol version we speak; the VPU acknowledges unasked */
	SMEM_MSG_VC_MEM_REQUEST_REPLY = 10, /* host: the answer to a VC_MEM_REQUEST */
	SMEM_MSG_RELEASED = 11,            /* VPU: its last reference to an import went */
	SMEM_MSG_VC_MEM_REQUEST = 12,      /* VPU: it asks for memory of the host */
};

#define SMEM_FROM_VPU 0x80000000u /* in the first word of a message the VPU sends unasked */

#define SMEM_MEM_NON_CACHED 1u /* an import's type: the VPU reads and writes it past its own cache */
#define SMEM_NAME_MAX 32       /* the name an import carries, for the VPU's own accounting */

struct smem_header
{
	__le32 type;     /* SMEM_MSG_* */
	__le32 trans_id; /* echoed in the answer; bit 31 clear */
};

/* IMPORT: memory of the host, as the VPU addresses it, wrapped in a handle */
struct smem_msg_import
{
	struct smem_header h;
	__le32 mem_type;  /* SMEM_MEM_NON_CACHED */
	__le32 addr;      /* the VPU's bus address of the memory */
	__le32 size;      /* its bytes */
	__le32 kernel_id; /* the host's cookie, returned in RELEASED */
	__le32 allocator; /* who imported it, for the VPU's accounting */
	char name[SMEM_NAME_MAX];
};

/* The answer to an IMPORT */
struct smem_msg_import_reply
{
	__le32 trans_id;
	__le32 handle; /* the import's handle, 0 if it failed */
};

/* FREE: the host lets go of an import */
struct smem_msg_free
{
	struct smem_header h;
	__le32 handle;
	__le32 mem; /* 0 */
};

/* CLIENT_VERSION */
struct smem_msg_version
{
	struct smem_header h;
	__le32 version; /* SMEM_CLIENT_VERSION */
};

/* RELEASED (from the VPU): an import is the host's again */
struct smem_msg_released
{
	__le32 cmd; /* SMEM_FROM_VPU | SMEM_MSG_RELEASED */
	__le32 addr;
	__le32 size;
	__le32 kernel_id;
	__le32 handle;
};

/* VC_MEM_REQUEST (from the VPU): memory wanted of the host */
struct smem_msg_mem_request
{
	__le32 cmd;      /* SMEM_FROM_VPU | SMEM_MSG_VC_MEM_REQUEST */
	__le32 trans_id; /* the VPU's, echoed in the reply */
	__le32 size;
	__le32 align;
	char name[SMEM_NAME_MAX];
	__le32 handle;
};

/* VC_MEM_REQUEST_REPLY: memory for the VPU, or none (address 0) */
struct smem_msg_mem_reply
{
	struct smem_header h;
	__le32 trans_id;  /* the request's */
	__le32 addr;      /* 0: refused */
	__le32 kernel_id; /* 0 with a refusal */
};

_Static_assert(sizeof(struct smem_msg_import) == 60, "import message layout");
_Static_assert(sizeof(struct smem_msg_released) == 20, "released message layout");
_Static_assert(sizeof(struct smem_msg_mem_request) == 52, "memory request layout");
_Static_assert(sizeof(struct smem_msg_mem_reply) == 20, "memory reply layout");

#endif /* SMEM_PROTO_H */
