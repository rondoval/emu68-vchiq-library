/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocodec_backend.c - the backends: found in LIBS:videocodec/, opened,
 * checked for the ABI, their codec tables cached, and chosen for a session
 * by the priority of their codec rows.
 *
 * The drawer is read with dos.library, which only a Process may call: the
 * scan happens at the first use by one.  A Task that comes first gets the
 * fallback backend by name, and the drawer is read at the next Process.  A
 * backend is kept open until the library's expunge, so its codec table is
 * read once.  Every function here runs with base->lock held, except
 * vc_backends_init() and vc_backends_free() (library init and expunge).
 */
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#ifdef __INTELLISENSE__
#include <clib/dos_protos.h>
#else
#include <proto/dos.h>
#endif

#include <memory.h>
#include <minlist.h>
#include <strutil.h>

#include <videocodec_private.h>

#define BE_FROM_NODE(node) ((struct vc_backend *)(node))
#define VC_CAPS_MAX 64 /* codec table rows read from one backend: a bound against one that never says "no more" */

/* ---- names ----------------------------------------------------------------------- */

/**
 * is_backend_name() - Tell whether a file in the drawer is a backend.
 * @name: the file name, without a path
 *
 * Return: TRUE if @name is longer than the suffix and ends in ".codec", in
 * any case, as AmigaDOS compares names.
 */
static BOOL is_backend_name(const char *name)
{
	ULONG len = (ULONG)strlen(name), suffix_len = (ULONG)sizeof(VIDEOCODEC_BACKEND_SUFFIX) - 1u;
	return len > suffix_len && _Stricmp((CONST_STRPTR)(name + len - suffix_len), (CONST_STRPTR)VIDEOCODEC_BACKEND_SUFFIX) == 0;
}

/**
 * name_matches() - Tell whether a name given by a program means a backend.
 * @backend: the backend's file name ("videocore.codec")
 * @name:    the name asked for, with or without ".codec", in any case
 *
 * "videocore", "VideoCore" and "videocore.CODEC" all mean videocore.codec.
 *
 * Return: TRUE if @name equals @backend, or @backend without its suffix,
 * ignoring case.
 */
static BOOL name_matches(const char *backend, const char *name)
{
	LONG len = (LONG)strlen(name);
	return _Strnicmp((CONST_STRPTR)backend, (CONST_STRPTR)name, len) == 0 &&
	       (backend[len] == 0 || _Stricmp((CONST_STRPTR)(backend + len), (CONST_STRPTR)VIDEOCODEC_BACKEND_SUFFIX) == 0);
}

/**
 * backend_by_name() - Find a backend in the list by its file name.
 * @base: the library base
 * @name: the file name, in any case
 *
 * Return: the backend, or NULL if none of that name is open.
 */
static struct vc_backend *backend_by_name(struct VideoCodecBase *base, const char *name)
{
	for (struct MinNode *node = base->backends.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
		if (name_matches(BE_FROM_NODE(node)->name, name))
			return BE_FROM_NODE(node);
	return NULL;
}

/* ---- one backend ----------------------------------------------------------------- */

/**
 * read_codec_table() - Copy a backend's codec table into the frontend's cache.
 * @base:    the library base
 * @backend: the backend; @backend->library and @backend->name set
 * @rows:    the rows it has, as VCB_GetInfo() reported (VCInfo.Codecs)
 *
 * Rows are kept as the backend gave them; the backend name is written when
 * a row is handed out, by VC_QueryCodec().  At most VC_CAPS_MAX rows are read; a backend that
 * answers fewer than it reported keeps the rows it gave (@backend->ncaps
 * counts them).
 *
 * Return: TRUE if the table is cached (an empty one included), FALSE if the
 * memory for it could not be had.
 */
static BOOL read_codec_table(struct VideoCodecBase *base, struct vc_backend *backend, ULONG rows)
{
	struct ExecBase *SysBase = base->SysBase;
	struct Library *VCBackendBase = backend->library;

	if (rows > VC_CAPS_MAX)
		rows = VC_CAPS_MAX;
	if (rows == 0)
		return TRUE;
	backend->caps = AllocVec(rows * sizeof(struct VCCodecInfo), MEMF_PUBLIC | MEMF_CLEAR);
	if (backend->caps == NULL)
	{
		Kprintf("[videocodec] %s: no memory for its codec table\n", backend->name);
		return FALSE;
	}
	for (ULONG i = 0; i < rows; i++)
	{
		struct VCCodecInfo *row = &backend->caps[i];
		row->Size = sizeof(struct VCCodecInfo);
		if (VCB_QueryCodec(i, row) != 0)
			break;
		backend->ncaps = i + 1u;
	}
	return TRUE;
}

/**
 * insert_sorted() - Put a backend into the list in name order.
 * @base:    the library base
 * @backend: the backend, not yet in the list
 *
 * Name order makes the choice between rows of equal priority the same every
 * time: vc_backend_choose() keeps the first one it finds.
 */
static void insert_sorted(struct VideoCodecBase *base, struct vc_backend *backend)
{
	struct ExecBase *SysBase = base->SysBase;
	struct MinNode *node = base->backends.mlh_Head;
	while (node->mln_Succ != NULL && strcmp(BE_FROM_NODE(node)->name, backend->name) < 0)
		node = node->mln_Succ;
	Insert((struct List *)&base->backends, (struct Node *)&backend->node, (struct Node *)node->mln_Pred);
}

/**
 * backend_drop() - Undo whatever of add_backend() was done.
 * @base:    the library base
 * @backend: the backend, not in the list; its library may be NULL (not opened)
 */
static void backend_drop(struct VideoCodecBase *base, struct vc_backend *backend)
{
	struct ExecBase *SysBase = base->SysBase;
	FreeVec(backend->caps);
	if (backend->library != NULL)
		CloseLibrary(backend->library);
	FreeVec(backend);
}

/**
 * backend_open() - Open a backend's library.
 * @base:    the library base
 * @backend: the backend, @backend->name set
 *
 * Opens "videocodec/<name>": Exec looks for it in every directory of LIBS:.
 *
 * Return: TRUE with @backend->library set, FALSE if the library does not open.
 */
static BOOL backend_open(struct VideoCodecBase *base, struct vc_backend *backend)
{
	struct ExecBase *SysBase = base->SysBase;

	char path[sizeof(VIDEOCODEC_BACKEND_DRAWER "/") + VC_BACKEND_NAME_MAX];
	strlcpy(path, VIDEOCODEC_BACKEND_DRAWER "/", sizeof(path));
	strlcpy(path + sizeof(VIDEOCODEC_BACKEND_DRAWER "/") - 1u, backend->name, VC_BACKEND_NAME_MAX);

	backend->library = OpenLibrary((CONST_STRPTR)path, 0);
	if (backend->library == NULL)
		KprintfT("[videocodec] %s: not opened\n", path);
	return backend->library != NULL;
}

/**
 * backend_handshake() - Check that a backend speaks this ABI, and learn its table size.
 * @backend: the backend, @backend->library open
 * @rows:    where the number of its codec rows (VCInfo.Codecs) goes
 *
 * Return: TRUE if VCB_GetInfo() answers with ABI major VCB_ABI_VERSION_MAJOR
 * and the row count, FALSE otherwise (logged).
 */
static BOOL backend_handshake(struct vc_backend *backend, ULONG *rows)
{
	struct Library *VCBackendBase = backend->library;
	struct VCInfo info;
	memset(&info, 0, sizeof(info)); /* a backend that fails or answers short leaves zeros, not stack garbage */
	info.Size = sizeof(info);

	LONG rc = VCB_GetInfo(&info);
	if (rc != 0 || !VC_HAS_FIELD(info.Size, struct VCInfo, Codecs) || VCB_ABI_MAJOR(info.ABIVersion) != VCB_ABI_VERSION_MAJOR)
	{
		Kprintf("[videocodec] %s: ABI %ld.%ld, not %ld.x (%ld)\n", backend->name, VCB_ABI_MAJOR(info.ABIVersion), VCB_ABI_MINOR(info.ABIVersion),
		        (ULONG)VCB_ABI_VERSION_MAJOR, rc);
		return FALSE;
	}
	*rows = info.Codecs;
	backend->abi = info.ABIVersion;
	return TRUE;
}

/**
 * add_backend() - Open one backend and keep it if it speaks the ABI.
 * @base: the library base
 * @name: the backend's file name in LIBS:videocodec/, without the drawer
 *
 * Opens it, checks its ABI, reads its codec table and inserts it in name
 * order.  A name already in the list is skipped; a backend that
 * fails a step is closed and left out, the step logging why.
 */
static void add_backend(struct VideoCodecBase *base, const char *name)
{
	struct ExecBase *SysBase = base->SysBase;
	if (strlen(name) >= VC_BACKEND_NAME_MAX || backend_by_name(base, name) != NULL)
		return;

	struct vc_backend *backend = AllocVec(sizeof(*backend), MEMF_PUBLIC | MEMF_CLEAR);
	if (backend == NULL)
		return;

	strlcpy(backend->name, name, sizeof(backend->name));
	ULONG rows;
	if (backend_open(base, backend) && backend_handshake(backend, &rows) && read_codec_table(base, backend, rows))
	{
		insert_sorted(base, backend);
		Kprintf("[videocodec] %s: %ld codecs\n", backend->name, backend->ncaps);
		return;
	}
	backend_drop(base, backend);
}

/* ---- the drawer ------------------------------------------------------------------ */

/**
 * scan_dir() - Add every backend file of one directory.
 * @base:    the library base
 * @DOSBase: dos.library, open
 * @dir:     a shared lock on the directory
 */
static void scan_dir(struct VideoCodecBase *base, struct DosLibrary *DOSBase, BPTR dir)
{
	struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
	if (fib == NULL)
		return;
	if (Examine(dir, fib) && fib->fib_DirEntryType > 0)
		while (ExNext(dir, fib))
			if (fib->fib_DirEntryType < 0 && is_backend_name((const char *)fib->fib_FileName))
				add_backend(base, (const char *)fib->fib_FileName);
	FreeDosObject(DOS_FIB, fib);
}

/**
 * scan_drawer() - Add every backend found in LIBS:videocodec/.
 * @base: the library base
 *
 * LIBS: is usually an assign of several directories, and each may have a
 * videocodec drawer: GetDeviceProc() walks them all, as the autodoc
 * describes.  A name found in two drawers is opened once, from wherever
 * Exec finds it first.  Must be called by a Process (dos.library); marks
 * the drawer scanned once dos.library opened, so a scan is not retried for
 * a drawer that is simply not there.
 */
static void scan_drawer(struct VideoCodecBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	static const char drawer[] = "LIBS:" VIDEOCODEC_BACKEND_DRAWER;
	struct DosLibrary *DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR) "dos.library", 36);
	if (DOSBase == NULL)
		return;

	BOOL found = FALSE;
	struct DevProc *devproc = NULL;
	while ((devproc = GetDeviceProc((CONST_STRPTR)drawer, devproc)) != NULL)
	{
		BPTR dir;
		if (devproc->dvp_Lock != 0)
		{
			/* one directory of the assign: the drawer relative to it */
			BPTR old = CurrentDir(devproc->dvp_Lock);
			dir = Lock((CONST_STRPTR)VIDEOCODEC_BACKEND_DRAWER, SHARED_LOCK);
			CurrentDir(old);
		}
		else
			dir = Lock((CONST_STRPTR)drawer, SHARED_LOCK); /* LIBS: is a volume or device, not an assign */
		if (dir != 0)
		{
			scan_dir(base, DOSBase, dir);
			UnLock(dir);
			found = TRUE;
		}
		if ((devproc->dvp_Flags & DVPF_ASSIGN) == 0)
			break;
	}
	FreeDeviceProc(devproc);
	if (!found)
		Kprintf("[videocodec] no %s drawer\n", drawer);
	CloseLibrary((struct Library *)DOSBase);
	base->scanned = TRUE;
}

/* ---- the list, for the rest of the library ------------------------------------------- */

/**
 * vc_backends_init() - Start with no backends and the drawer not scanned.
 * @base: the library base, at library init
 */
void vc_backends_init(struct VideoCodecBase *base)
{
	_NewMinList(&base->backends);
	base->scanned = FALSE;
}

/**
 * vc_backends_free() - Close every backend and free the cache.
 * @base: the library base, at expunge (no session is open)
 *
 * Leaves the list empty and the drawer unscanned, as vc_backends_init().
 */
void vc_backends_free(struct VideoCodecBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct MinNode *node;
	while ((node = (struct MinNode *)RemHead((struct List *)&base->backends)) != NULL)
		backend_drop(base, BE_FROM_NODE(node));
	base->scanned = FALSE;
}

/**
 * vc_backends_idle() - The last opener is gone: every backend lets go of what it keeps between calls.
 * @base: the library base, in the closing task (it may wait)
 *
 * VCB_Idle() of each backend.
 */
void vc_backends_idle(struct VideoCodecBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->lock);
	for (struct MinNode *node = base->backends.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct Library *VCBackendBase = BE_FROM_NODE(node)->library;
		VCB_Idle();
	}
	ReleaseSemaphore(&base->lock);
}

/**
 * vc_backends_load() - Make the backend list ready for use.
 * @base: the library base, lock held
 *
 * Scans the drawer if that has not been done and the caller is a Process.
 * If the list is still empty (a Task caller, or an empty drawer), opens the
 * fallback backend by name, which needs no DOS; the drawer is then scanned
 * at the next call from a Process.
 */
void vc_backends_load(struct VideoCodecBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	if (base->scanned)
		return;
	if (FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS)
		scan_drawer(base);
	if (base->backends.mlh_Head->mln_Succ == NULL)
		add_backend(base, VC_FALLBACK_BACKEND);
}

/**
 * vc_format_mask() - A format's bit in a row's format masks.
 * @format: VCF_*, 0 for none
 *
 * Return: VCF_MASK(@format); 0 for none, and for a number past the masks' 32
 * bits, which no row can hold.
 */
ULONG vc_format_mask(ULONG format)
{
	return format != 0 && format < 32u ? VCF_MASK(format) : 0u;
}

/**
 * vc_row_takes() - Whether a row reads and writes the pictures a choice asks for.
 * @row:  the row
 * @want: what is asked; a mask of 0 asks nothing
 *
 * Return: TRUE when the row reads one of @want->reads and writes one of
 * @want->writes.
 */
BOOL vc_row_takes(const struct VCCodecInfo *row, const struct vc_choice *want)
{
	return (want->reads == 0 || (row->InputFormats & want->reads) != 0) &&
	       (want->writes == 0 || (row->OutputFormats & want->writes) != 0);
}

/**
 * row_serves() - Whether a row is one of a choice's at all: its codec and kind, the use, the FourCC.
 * @row:  the row
 * @want: what is asked
 * @tag:  @want->tag in upper case (vc_tag_fold())
 *
 * A row with a Tag serves only that FourCC, in any case: a backend that
 * decodes one form of a codec (MS-MPEG4 v3, VC-1's WMV3) is not handed
 * another.  A row without one serves every form.
 *
 * Return: TRUE when it serves.
 */
static BOOL row_serves(const struct VCCodecInfo *row, const struct vc_choice *want, ULONG tag)
{
	if (row->Codec != want->codec || row->Kind != want->kind || (row->Flags & want->use) == 0)
		return FALSE;
	return row->Tag == 0 || (want->tag != 0 && vc_tag_fold(row->Tag) == tag);
}

/**
 * ranks_above() - Whether one serving row beats another for a choice.
 * @row:  the row looked at
 * @best: the best one so far
 * @want: what is asked
 *
 * In this order: a row that takes the pictures asked for (vc_row_takes()),
 * so that a converter or encoder of lower priority serves what a higher one
 * cannot; the higher priority; a row of the session's own FourCC over one of
 * every form.  A tie keeps @best, the backend first in name order.
 *
 * Return: TRUE when @row ranks above @best.
 */
static BOOL ranks_above(const struct VCCodecInfo *row, const struct VCCodecInfo *best, const struct vc_choice *want)
{
	BOOL takes = vc_row_takes(row, want);
	if (takes != vc_row_takes(best, want))
		return takes;
	if (row->Priority != best->Priority)
		return row->Priority > best->Priority;
	return row->Tag != 0 && best->Tag == 0;
}

/**
 * vc_backend_choose() - The backend for a session, an image call or a chain's converter, and the row that won.
 * @base: the library base, lock held, list loaded
 * @want: the codec, its FourCC, the kind, the use, the pictures read and written
 * @name: the backend a program asked for (VCA_Backend), with or without
 *        ".codec", in any case; NULL to leave the choice to the library
 * @row:  where the winning row goes (NULL when none); may be NULL
 *
 * Of every row that serves @want (row_serves()), the one that ranks highest
 * (ranks_above()); with @name, only that backend's rows.  A row that does
 * not take the pictures asked for still wins when no row does: its
 * backend's refusal says why, as before there was a choice by format.  A
 * caller that must have the pictures taken checks the row with
 * vc_row_takes().
 *
 * Return: the backend, or NULL when no row serves @want.
 */
struct vc_backend *vc_backend_choose(struct VideoCodecBase *base, const struct vc_choice *want, const char *name,
                                     const struct VCCodecInfo **row)
{
	struct vc_backend *best = NULL;
	const struct VCCodecInfo *best_row = NULL;
	ULONG tag = vc_tag_fold(want->tag);
	for (struct MinNode *node = base->backends.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct vc_backend *backend = BE_FROM_NODE(node);
		if (name != NULL && !name_matches(backend->name, name))
			continue;
		for (ULONG i = 0; i < backend->ncaps; i++)
		{
			const struct VCCodecInfo *candidate = &backend->caps[i];
			if (!row_serves(candidate, want, tag))
				continue;
			if (best_row == NULL || ranks_above(candidate, best_row, want))
			{
				best = backend;
				best_row = candidate;
			}
		}
	}
	if (row != NULL)
		*row = best_row;
	return best;
}

/* ---- LVOs -------------------------------------------------------------------------- */

/**
 * VC_QueryCodec() - One row of the merged codec table (LVO).
 * @index: the row, 0 upward; rows come backend by backend, in name order
 * @info:  the caller's structure, Size set to the size it was compiled with
 * @base:  the library base (A6)
 *
 * Copies the row into @info up to the smaller of the caller's Size and the
 * library's, then the backend's file name into Backend (for VCA_Backend),
 * if the caller's structure has it.  Size ends up as what was copied.
 *
 * Return: 0, or VCERR_BADPARAM past the last row or for a bad @info.
 */
LONG VC_QueryCodec(ULONG index asm("d0"), struct VCCodecInfo *info asm("a0"), struct VideoCodecBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (info == NULL || !VC_HAS_FIELD(info->Size, struct VCCodecInfo, Kind))
		return VCERR_BADPARAM;

	LONG rc = VCERR_BADPARAM;
	ObtainSemaphore(&base->lock);
	vc_backends_load(base);
	for (struct MinNode *node = base->backends.mlh_Head; node->mln_Succ != NULL && rc != 0; node = node->mln_Succ)
	{
		struct vc_backend *backend = BE_FROM_NODE(node);
		if (index < backend->ncaps)
		{
			ULONG size = info->Size < sizeof(*info) ? info->Size : (ULONG)sizeof(*info);
			memcpy(info, &backend->caps[index], size);
			info->Size = size;
			if (VC_HAS_FIELD(size, struct VCCodecInfo, Backend))
				strlcpy((char *)info->Backend, backend->name, sizeof(info->Backend));
			rc = 0;
		}
		else
			index -= backend->ncaps;
	}
	ReleaseSemaphore(&base->lock);
	return rc;
}

/**
 * VC_GetBackendInfo() - One backend's state and counters (LVO).
 * @index: the backend, 0 upward, in name order
 * @info:  the caller's structure, Size set to the size it was compiled with
 * @base:  the library base (A6)
 *
 * The backend fills @info, its counters into the caller's Counters as far as
 * MaxCounters reaches; the frontend then writes Backend, the file name, when
 * the caller's structure has it.
 *
 * Return: 0, or VCERR_BADPARAM past the last backend or for a bad @info,
 * or the backend's error.
 */
LONG VC_GetBackendInfo(ULONG index asm("d0"), struct VCInfo *info asm("a0"), struct VideoCodecBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (info == NULL || !VC_HAS_FIELD(info->Size, struct VCInfo, ABIVersion))
		return VCERR_BADPARAM;

	LONG rc = VCERR_BADPARAM;
	ObtainSemaphore(&base->lock);
	vc_backends_load(base);
	struct MinNode *node = base->backends.mlh_Head;
	while (node->mln_Succ != NULL && index-- != 0)
		node = node->mln_Succ;
	if (node->mln_Succ != NULL)
	{
		struct vc_backend *backend = BE_FROM_NODE(node);
		struct Library *VCBackendBase = backend->library;
		rc = VCB_GetInfo(info);
		if (rc == 0 && VC_HAS_FIELD(info->Size, struct VCInfo, Backend))
			strlcpy((char *)info->Backend, backend->name, sizeof(info->Backend));
	}
	ReleaseSemaphore(&base->lock);
	return rc;
}
