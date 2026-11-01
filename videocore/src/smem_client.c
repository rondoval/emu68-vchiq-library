/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * smem_client.c - the SMEM client: memory of ours imported into the VPU and
 * let go again (smem_client.h has the model, smem_proto.h the messages).
 */
#include <byteorder.h>
#include <memory.h>

#include <videocore_private.h>
#include <smem_proto.h>

/* The name each import carries, for the VPU's own accounting */
static const char import_name[] = "videocore.codec";

_Static_assert(sizeof(import_name) <= SMEM_NAME_MAX, "the name must fit an import");
_Static_assert(SMEM_IMPORTS_MAX <= 256, "an entry's index is the cookie's low byte");

/* ---- the table ----------------------------------------------------------------- */

/**
 * entry_cookie() - The cookie that names an entry to the VPU: its index and generation.
 * @smem:  the client
 * @entry: an entry of its table
 *
 * Bit 31 stays clear: the VPU sets it in what it sends unasked.
 *
 * Return: the cookie, never 0 (generations start at 1).
 */
static u32 entry_cookie(const struct smem_client *smem, const struct smem_import *entry)
{
	return ((u32)entry->generation << 8) | (u32)(entry - smem->imports);
}

/**
 * entry_by_cookie() - The entry a cookie names.
 * @smem:   the client
 * @cookie: the cookie, as the VPU echoed it
 *
 * Return: the entry, NULL if the cookie names none that is in use.
 */
static struct smem_import *entry_by_cookie(struct smem_client *smem, u32 cookie)
{
	u32 index = cookie & 0xffu;
	if (index >= SMEM_IMPORTS_MAX)
		return NULL;
	struct smem_import *entry = &smem->imports[index];
	return entry->state != SMEM_ENTRY_FREE && entry->generation == (u16)(cookie >> 8) ? entry : NULL;
}

/**
 * entry_free() - An entry back to unused.
 * @smem:  the client
 * @entry: the entry, in use
 */
static void entry_free(struct smem_client *smem, struct smem_import *entry)
{
	entry->state = SMEM_ENTRY_FREE;
	entry->handle = 0;
	smem->live--;
}

/**
 * entry_lose() - An entry the VPU may still hold: out of use until the service closes.
 * @entry: the entry
 * @why:   for the log
 */
static void entry_lose(struct smem_import *entry, const char *why)
{
	(void)why; /* only the log reads it */
	Kprintf("[videocore] import %08lx (%ld bytes) %s: the VPU may still hold it\n", entry->addr, entry->size, why);
	entry->state = SMEM_ENTRY_LOST;
}

/**
 * header_set() - Fill a message's header.
 * @header:   the header
 * @type:     SMEM_MSG_*
 * @trans_id: what the answer echoes
 */
static void header_set(struct smem_header *header, u32 type, u32 trans_id)
{
	header->type = le32(type);
	header->trans_id = le32(trans_id);
}

/* ---- imports (callers) --------------------------------------------------------------- */

/* An import batch: what smem_import() waits for */
struct import_batch
{
	struct smem_client *smem;
	const u32 *ids;
	u32 count;
};

/**
 * batch_answered() - Whether the VPU answered every import of a batch (a link_wait() condition).
 * @base:  the library base
 * @batch: the batch
 *
 * Return: TRUE once none is still importing, or the service went.
 */
static BOOL batch_answered(APTR base, APTR batch)
{
	(void)base;
	const struct import_batch *b = batch;
	for (u32 i = 0; i < b->count; i++)
		if (b->ids[i] != 0 && b->smem->imports[b->ids[i] - 1u].state == SMEM_ENTRY_IMPORTING)
			return FALSE;
	return TRUE;
}

/**
 * batch_reserve() - Free entries for a batch of imports.
 * @smem:  the client
 * @count: how many
 * @ids:   where the entries' ids go
 *
 * Return: TRUE with every id set; FALSE with every id 0 (not enough free, or the service down).
 */
static BOOL batch_reserve(struct smem_client *smem, u32 count, u32 *ids)
{
	u32 found = 0;
	for (u32 i = 0; i < SMEM_IMPORTS_MAX && found < count && smem->up; i++)
		if (smem->imports[i].state == SMEM_ENTRY_FREE)
			ids[found++] = i + 1u;
	if (found == count)
		return TRUE;
	memset(ids, 0, count * sizeof(*ids));
	return FALSE;
}

/**
 * import_send() - Start one import: the entry set up, its IMPORT sent.
 * @base:  the library base
 * @entry: a free entry
 * @range: the memory
 *
 * Return: 0, or the link's error with the entry FAILED.
 */
static s32 import_send(struct VideoCoreBase *base, struct smem_import *entry, const struct smem_range *range)
{
	struct smem_client *smem = &base->smem;
	if (++smem->generation == 0)
		smem->generation = 1;
	entry->state = SMEM_ENTRY_IMPORTING;
	entry->generation = smem->generation;
	entry->addr = (u32)range->data;
	entry->size = range->size;
	entry->handle = 0;
	smem->live++;

	u32 cookie = entry_cookie(smem, entry);
	struct smem_msg_import msg;
	memset(&msg, 0, sizeof(msg));
	header_set(&msg.h, SMEM_MSG_IMPORT, cookie);
	msg.mem_type = le32(SMEM_MEM_NON_CACHED);
	msg.addr = le32(range->bus);
	msg.size = le32(entry->size);
	msg.kernel_id = le32(cookie);
	memcpy(msg.name, import_name, sizeof(import_name));
	s32 status = link_send_smem(base, &msg, sizeof(msg), cookie, TRUE);
	if (status != 0)
		entry->state = SMEM_ENTRY_FAILED;
	return status;
}

/**
 * batch_undo() - A batch that did not wholly succeed: let go of what went through, free the rest.
 * @base:  the library base
 * @count: the batch's size
 * @ids:   its entries' ids, 0 for one never started; all 0 on return
 */
static void batch_undo(struct VideoCoreBase *base, u32 count, u32 *ids)
{
	struct smem_client *smem = &base->smem;
	for (u32 i = 0; i < count; i++)
	{
		if (ids[i] == 0)
			continue;
		struct smem_import *entry = &smem->imports[ids[i] - 1u];
		if (entry->state == SMEM_ENTRY_LIVE)
			smem_release(base, ids[i]);
		else if (entry->state == SMEM_ENTRY_FAILED)
			entry_free(smem, entry);
		else if (entry->state == SMEM_ENTRY_IMPORTING)
			entry_lose(entry, "never answered");
		ids[i] = 0;
	}
}

/**
 * smem_import() - Import ranges into the VPU, one round trip for all of them.
 * @base:   the library base, mmal_lock held (released while waiting)
 * @ranges: the memory, each range where the VPU can work in it (zero_copy_qualifies())
 * @count:  how many
 * @ids:    where the imports go, one per range
 *
 * Every IMPORT goes out back to back, then the answers are waited for at
 * once.  All or nothing: if one fails, the others are let go again.  An
 * answer that never comes takes the service as gone.
 *
 * Return: 0 with every id set, or LINK_ERR_* with every id 0.
 */
s32 smem_import(struct VideoCoreBase *base, const struct smem_range *ranges, u32 count, u32 *ids)
{
	struct smem_client *smem = &base->smem;
	if (!batch_reserve(smem, count, ids))
		return smem->up ? LINK_ERR_NOSPACE : LINK_ERR_STATE;

	PERF_T0(t0);
	s32 status = 0;
	u32 sent = 0;
	while (sent < count && status == 0)
	{
		status = import_send(base, &smem->imports[ids[sent] - 1u], &ranges[sent]);
		sent++;
	}
	for (u32 i = sent; i < count; i++)
		ids[i] = 0; /* never started: still free */
	struct import_batch batch = {.smem = smem, .ids = ids, .count = count};
	s32 waited = link_wait(base, batch_answered, &batch, LINK_WAIT_MS);
	if (status == 0)
		status = waited;

	BOOL all = status == 0;
	for (u32 i = 0; i < count && all; i++)
		all = smem->imports[ids[i] - 1u].state == SMEM_ENTRY_LIVE;
	if (all)
	{
		PERF_ADD(&base->perf, VCPERF_IMPORT, t0);
		return 0;
	}
	batch_undo(base, count, ids);
	if (waited == LINK_ERR_TIMEOUT)
	{
		Kprintf("[videocore] the VPU did not answer an import in %ld ms\n", (ULONG)LINK_WAIT_MS);
		smem_lost(base);
	}
	return status != 0 ? status : LINK_ERR_REMOTE;
}

/**
 * smem_handle() - The VPU's handle of an import.
 * @base: the library base
 * @id:   the import, from smem_import()
 *
 * Return: the handle.
 */
u32 smem_handle(const struct VideoCoreBase *base, u32 id)
{
	return base->smem.imports[id - 1u].handle;
}

/**
 * smem_release() - Let go of an import.
 * @base: the library base, mmal_lock held
 * @id:   the import, from smem_import(); its port no longer runs
 *
 * Its FREE goes out and nothing waits: smem_released_wait() waits for the
 * VPU's RELEASED, which ends the entry.  Without the version 2 handshake the
 * VPU reports nothing, and the free is taken as final at once.  Never waits.
 */
void smem_release(struct VideoCoreBase *base, u32 id)
{
	struct smem_client *smem = &base->smem;
	struct smem_import *entry = &smem->imports[id - 1u];
	if (entry->state != SMEM_ENTRY_LIVE)
		return;

	u32 cookie = entry_cookie(smem, entry);
	struct smem_msg_free msg;
	header_set(&msg.h, SMEM_MSG_FREE, cookie);
	msg.handle = le32(entry->handle);
	msg.mem = 0;
	if (!smem->up || link_send_smem(base, &msg, sizeof(msg), cookie, FALSE) != 0)
		entry_lose(entry, "could not be freed");
	else if (smem->releases_reported)
		entry->state = SMEM_ENTRY_RELEASING;
	else
		entry_free(smem, entry);
}

/**
 * imports_released() - Whether no import waits for its RELEASED (a link_wait() condition).
 * @base: the library base
 * @arg:  unused
 *
 * Return: TRUE once every released import is reported back.
 */
static BOOL imports_released(APTR base, APTR arg)
{
	(void)arg;
	const struct smem_client *smem = &((struct VideoCoreBase *)base)->smem;
	for (u32 i = 0; i < SMEM_IMPORTS_MAX; i++)
		if (smem->imports[i].state == SMEM_ENTRY_RELEASING)
			return FALSE;
	return TRUE;
}

/**
 * smem_released_wait() - Sleep until the VPU reported back every import let go.
 * @base: the library base, mmal_lock held (released while waiting), from a task
 *
 * The memory of an import is its owner's again only then.  What does not
 * come back within LINK_WAIT_MS is logged and kept out of use.
 */
void smem_released_wait(struct VideoCoreBase *base)
{
	struct smem_client *smem = &base->smem;
	if (imports_released(base, NULL))
		return;
	PERF_T0(t0);
	if (link_wait(base, imports_released, NULL, LINK_WAIT_MS) != 0)
	{
		for (u32 i = 0; i < SMEM_IMPORTS_MAX; i++)
			if (smem->imports[i].state == SMEM_ENTRY_RELEASING)
				entry_lose(&smem->imports[i], "never reported released");
	}
	PERF_ADD(&base->perf, VCPERF_RELEASE, t0);
}

/* ---- the link's ------------------------------------------------------------------------ */

/**
 * smem_up() - The service opened: tell the VPU our protocol version.
 * @base: the library base, mmal_lock held, from a task
 *
 * The VPU acknowledges unasked, and before it answers anything sent after:
 * the first imports' answers find releases_reported set.
 */
void smem_up(struct VideoCoreBase *base)
{
	struct smem_client *smem = &base->smem;
	smem->up = TRUE;
	smem->releases_reported = FALSE;

	struct smem_msg_version msg;
	header_set(&msg.h, SMEM_MSG_CLIENT_VERSION, 0);
	msg.version = le32(SMEM_CLIENT_VERSION);
	if (link_send_smem(base, &msg, sizeof(msg), 0, TRUE) != 0)
		Kprintf("[videocore] the SMEM version did not go: no reports of released imports\n");
}

/**
 * smem_closing() - The service is about to close: what is still imported is logged and forgotten.
 * @base: the library base, mmal_lock held
 *
 * Every port is gone by now, so nothing should be left; whatever is, the
 * VPU never uses again (no component runs).
 */
void smem_closing(struct VideoCoreBase *base)
{
	struct smem_client *smem = &base->smem;
	if (smem->live != 0)
		Kprintf("[videocore] %ld imports still held at the SMEM close\n", smem->live);
	if (smem->stray_messages != 0)
		Kprintf("[videocore] %ld SMEM messages matched nothing\n", smem->stray_messages);
	for (u32 i = 0; i < SMEM_IMPORTS_MAX; i++)
		smem->imports[i].state = SMEM_ENTRY_FREE;
	smem->live = 0;
	smem->stray_messages = 0;
	smem->up = FALSE;
}

/**
 * import_answered() - The VPU answered an IMPORT.
 * @smem:  the client
 * @reply: the answer
 */
static void import_answered(struct smem_client *smem, const struct smem_msg_import_reply *reply)
{
	struct smem_import *entry = entry_by_cookie(smem, le32(reply->trans_id));
	if (entry == NULL || entry->state != SMEM_ENTRY_IMPORTING)
	{
		smem->stray_messages++;
		Kprintf("[videocore] an import answer that matched nothing: %08lx\n", le32(reply->trans_id));
		return;
	}
	entry->handle = le32(reply->handle);
	entry->state = entry->handle != 0 ? SMEM_ENTRY_LIVE : SMEM_ENTRY_FAILED;
	if (entry->handle == 0)
		Kprintf("[videocore] the VPU refused the import of %08lx (%ld bytes)\n", entry->addr, entry->size);
	KprintfT("[videocore] import %08lx+%ld: handle %08lx\n", entry->addr, entry->size, entry->handle);
}

/**
 * import_released() - The VPU's last reference to an import went (RELEASED).
 * @smem:     the client
 * @released: the message
 */
static void import_released(struct smem_client *smem, const struct smem_msg_released *released)
{
	struct smem_import *entry = entry_by_cookie(smem, le32(released->kernel_id));
	if (entry == NULL ||
	    (entry->state != SMEM_ENTRY_RELEASING && entry->state != SMEM_ENTRY_LOST && entry->state != SMEM_ENTRY_LIVE))
	{
		smem->stray_messages++;
		Kprintf("[videocore] a release that matched nothing: %08lx\n", le32(released->kernel_id));
		return;
	}
	if (entry->state == SMEM_ENTRY_LIVE)
		Kprintf("[videocore] the VPU let go of import %08lx unasked\n", entry->addr); /* nothing ever sends it again */
	entry_free(smem, entry);
}

/**
 * memory_refused() - The VPU asked for memory of ours: refuse it, as an allocation that failed.
 * @base:    the library base
 * @request: the request
 *
 * The receive task cannot wait for a request: with none free the refusal
 * does not go, which is logged.
 */
static void memory_refused(struct VideoCoreBase *base, const struct smem_msg_mem_request *request)
{
	char name[SMEM_NAME_MAX + 1];
	memcpy(name, request->name, SMEM_NAME_MAX);
	name[SMEM_NAME_MAX] = '\0';
	Kprintf("[videocore] the VPU asked for %ld bytes (%s), refused\n", le32(request->size), name);

	struct smem_msg_mem_reply reply;
	header_set(&reply.h, SMEM_MSG_VC_MEM_REQUEST_REPLY, 0);
	reply.trans_id = request->trans_id;
	reply.addr = 0;
	reply.kernel_id = 0;
	if (link_send_smem(base, &reply, sizeof(reply), 0, FALSE) != 0)
		Kprintf("[videocore] the refusal did not go\n");
}

/**
 * smem_message() - A message from the VPU on the SMEM service.
 * @base: the library base, mmal_lock held (the receive task)
 * @data: the message
 * @size: its bytes
 */
void smem_message(struct VideoCoreBase *base, const u8 *data, u32 size)
{
	struct smem_client *smem = &base->smem;
	union
	{
		__le32 first;
		struct smem_msg_import_reply import;
		struct smem_msg_released released;
		struct smem_msg_mem_request request;
	} message;
	u32 kept = umin(size, sizeof(message));
	memset(&message, 0, sizeof(message));
	memcpy(&message, data, kept); /* out of the device's buffer, aligned for the fields */

	u32 first = le32(message.first);
	u32 type = first & ~SMEM_FROM_VPU;
	if ((first & SMEM_FROM_VPU) == 0 && size >= sizeof(message.import))
		import_answered(smem, &message.import);
	else if ((first & SMEM_FROM_VPU) && type == SMEM_MSG_CLIENT_VERSION)
	{
		smem->releases_reported = TRUE;
		KprintfT("[videocore] SMEM: the VPU takes protocol version %ld\n", (ULONG)SMEM_CLIENT_VERSION);
	}
	else if ((first & SMEM_FROM_VPU) && type == SMEM_MSG_RELEASED && size >= sizeof(message.released))
		import_released(smem, &message.released);
	else if ((first & SMEM_FROM_VPU) && type == SMEM_MSG_VC_MEM_REQUEST && size >= sizeof(message.request))
		memory_refused(base, &message.request);
	else
	{
		smem->stray_messages++;
		Kprintf("[videocore] an SMEM message that matched nothing: %08lx, %ld bytes\n", first, size);
	}
}

/**
 * smem_send_failed() - A message of ours did not reach the VPU.
 * @base:   the library base, mmal_lock held (the receive task)
 * @cookie: the entry's cookie the message carried, 0 for none
 *
 * An import that did not go failed; a free that did not go leaves the
 * import with the VPU.
 */
void smem_send_failed(struct VideoCoreBase *base, u32 cookie)
{
	struct smem_client *smem = &base->smem;
	struct smem_import *entry = cookie != 0 ? entry_by_cookie(smem, cookie) : NULL;
	if (entry == NULL)
		return;
	if (entry->state == SMEM_ENTRY_IMPORTING)
		entry->state = SMEM_ENTRY_FAILED;
	else if (entry->state == SMEM_ENTRY_RELEASING)
		entry_lose(entry, "free not sent");
}

/**
 * smem_lost() - The service went: nothing more is imported, and what waits gives up.
 * @base: the library base, mmal_lock held
 *
 * The VPU closed the service, the whole link went (link_dead()), or an
 * import's answer never came.
 */
void smem_lost(struct VideoCoreBase *base)
{
	struct smem_client *smem = &base->smem;
	if (!smem->up)
		return;
	Kprintf("[videocore] the SMEM service is gone: no more imports\n");
	smem->up = FALSE;
	for (u32 i = 0; i < SMEM_IMPORTS_MAX; i++)
	{
		struct smem_import *entry = &smem->imports[i];
		if (entry->state == SMEM_ENTRY_IMPORTING)
			entry->state = SMEM_ENTRY_FAILED;
		else if (entry->state == SMEM_ENTRY_RELEASING)
			entry_lose(entry, "lost with the service");
	}
}
