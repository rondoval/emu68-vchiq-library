# emu68-vchiq-library

Lets 68k code use the VideoCore VPU firmware. The stack-wide rules in the
superbuild's `AGENTS.md` apply (container builds only, `__NOLIBBASE__` with a
local `SysBase` in freestanding code, C99 declarations at first use, zero
warnings, the user owns git).

| Binary | Source | Installed in | What it is |
|---|---|---|---|
| `vchiq.device` | `vchiq/` | `DEVS:` | the VCHIQ transport, one unit per service |
| `videocodec.library` | `videocodec/` | `LIBS:` | the public codec API; knows no hardware |
| `videocore.codec` | `videocore/` | `LIBS:videocodec/` | the MMAL backend |
| `videocore.audio` | `ahi/` | `DEVS:AHI/`, mode file `DEVS:AudioModes/VIDEOCORE` | AHI sub-driver on the firmware's `AUDS` service |
| `vcjpeg.datatype` | `datatype/` | `SYS:Classes/DataTypes/`, descriptor `SYS:Storage/DataTypes/VCJPEG` | `picture.datatype` subclass; its descriptor is added from `S:User-Startup`, after the system's |

## Docs

Each document has one reader; put new text where its reader looks.

| File | Reader | Holds |
|---|---|---|
| `docs/architecture.md` | whoever changes the design | how it works and why, and the firmware's limits as design rules. Read it before touching protocol code. |
| `docs/firmware-facts.md` | whoever touches the firmware path | what the firmware does, as measured, and the sources. New measurements go here. |
| `developer-guide.md` | people using or working on the component | the API, backend ABI, tools, building, the test suite |
| `README.md`, `RELEASE-NOTES.md` | end users | installing and using it; the current release only |
| this file | agents | what must not break; no history, no how-it-works |

## Build and validate

From the stack root, `./scripts/docker-build.sh --target emu68-vchiq-library`
or `./build.sh --build`. Zero warnings.

From the component root:

- After a harness change: `tests/.venv/bin/pytest tests/unit` (host only).
- On the hardware: `tests/.venv/bin/pytest tests -m smoke` after a change, the
  full suite before a release, on the suite's build flavour (developer-guide
  8.2). The machines are shared: the user runs these or says when to. Stop your
  own serial capture and anything polling the Amiga's agent first; the suite
  needs both to itself.
- Report the installed files a run replaces (`DEVS:vchiq.device`,
  `LIBS:videocodec.library`, `LIBS:videocodec/videocore.codec`, `C:VCInfo`,
  `SYS:Tools/Commodities/VCGrab` and its icon, `DEVS:AHI/videocore.audio`,
  `DEVS:AudioModes/VIDEOCORE`, `SYS:Classes/DataTypes/vcjpeg.datatype`,
  `SYS:Storage/DataTypes/VCJPEG`) and
  whether `baselines.json` changed.

## Rules

1. **BSD-3-Clause.** GPL code is forbidden. The `tools/vcgrab/mkv` submodule
   (mkv-writer) comes from libvpx's BSD libmkv and keeps its notice.
2. **Layering.** The frontend includes no vchiq or MMAL header (its CMake gives
   it none). The backend parses no tags (it gets `struct VCSessionParams`).
   `videocore/`, `ahi/` and the tools include only `devices/vchiq.h` of
   `vchiq/` (VCInfo's window dump and MMAL inventory, and VCTest's
   `vchiq/vchiq_test.h`, excepted). `datatype/` includes only
   `libraries/videocodec.h` and the shared `image_info.h`, like a program.
3. **The ABIs are append-only.** New structure fields at the end behind `Size`,
   new functions at the end of the SFD; either bumps `VCB_ABI_VERSION_MINOR`. A
   reader checks the peer's `Size` with `VC_HAS_FIELD()` before reading a later
   field (a codec row without `Priority` is software).
4. **No payload copies.** Input objects are sent by reference, output objects
   written by the hardware in place. Zero copy is the MMAL client's decision per
   port at `mmal_port_attach()`, never the session layer's.
5. **One version for the component**, set in the root `CMakeLists.txt`; each
   binary takes `${CMAKE_PROJECT_VERSION}`. No source, header fallback or SFD
   `==id` carries a number. Protocol versions (VCHIQ, MMAL, `VCB_ABI_VERSION`)
   are separate.
6. **Functions carry kernel-doc comments** (summary, every parameter,
   `Return:`). Documents stay ASCII.
7. **`tools/VCPlay.c` and `tools/VCShow.c` are the examples third parties
   copy:** no instrumentation, no options, nothing of the stack but the
   library's headers. They ship as source (with `tools/examples.readme`) and
   must build from the archive's `Developer/Examples/videocodec`.
8. **The rigs print facts, the host judges them.** `tools/vctest/` and
   `tools/VPUPlay.c` speak the grammar of `tools/common/kv.h`; a test never
   greps prose. VPUPlay is a measurement rig: users play video with MintVID.
9. **Test hooks only in test builds.** `vchiq/vchiq_test.h` commands exist only
   with `VCHIQ_TEST_HOOKS` (the stack's `EMU68_VCHIQ_TEST_HOOKS`, which the
   suite's build sets and no release does).
10. **Tests are pytest functions in `tests/`** (developer-guide 8.7): inputs from
    fixtures, never from another test's side effects; temporary files in
    `RAM:hw` only; measured numbers through `baselines.check()`; every module
    ends in `test_nothing_left`.

## The firmware: what never to do

Each of these has happened. The firmware refuses almost nothing, and most of
these leave it dead until a Pi restart (architecture.md section 9).

- Never send a second init tag: the VPU answers `0xFFFFFFFF` and never takes it.
- Never flush the decoder's end of an opaque (`OPQV`) tunnel: the VPU's MMAL
  service dies. Flush its ISP end.
- Never close or fail the image decoder mid-picture: MMAL stays dead. Every
  check reads the whole header and every refusal comes before the session opens,
  the host's frames included: `still_fits()` takes the pipeline's and holds it to
  the format change, as any task can take memory that is only checked.
- Never hand it a run-length TGA: that takes the whole firmware down. Refusals
  from the header are the only guard.
- Never let image calls overlap: each sees the codec heap before the others take
  theirs, the heap runs out, and a decoder that fails holding a picture can stop
  the firmware.
- Never rely on the decoder to refuse a stream: past its limits it gives wrong
  pictures or none, without a word. The backend's checks are the only guard.
- Enable only a tunnel's input end, then read the far end's state back: the VPU
  enables and disables both ends on the first action and refuses the second.
  Start the ISP's output, frames out, before that end (`isp_tunnel_start()`), or
  the first picture can be lost.

## Invariants

What a change must not break, per binary. The reasons are in
`docs/architecture.md`, in the sections named.

### vchiq.device (sections 3, 4)

- Every window access goes through `le32()`/`le16()` on `volatile` pointers. No
  cache maintenance on the slot area; `cache_pre_dma()`/`cache_post_dma()` on
  bulk payloads and on imported buffers only.
- vchiq must stay the first `cma.resource` client after a reset: otherwise the
  window moves while the VPU still writes the old one.
- The window's record outlives a VPU restart, so `vchiq_connect()` trusts it
  only after the VPU has answered on it (`probe_alive()`). The session API is
  `vchiq_connect()` and `vchiq_shutdown()`; the steps stay private to
  `vchiq_session.c`.
- Every ring access is under `base->lock` and inside `vchiq_ring_enter()` /
  `vchiq_ring_leave()`. The reset path cannot lock: it reads the busy count.
- The reset guard cannot `Wait()`: it clears `ctx->timer` and spins.
- `videocore.codec`'s reset guard must run before the device's: a service's close
  leaves its components on the VPU, and the next `SMEM` open then hangs the link
  (`sessions_reset()`). It goes in after the service opens and out before it closes.
- `BeginIO` only queues on the unit and kicks the task; only the task's round
  touches the ring. The device never calls into a client or waits for one:
  replies are `ReplyMsg()`s.
- Never merge `sendq` (writes and bulk transmits) and `bulkreadq` (bulk
  receives), and keep each in order: a message and its bulk must never be
  reordered.
- OpenDevice and CloseDevice wait through `drv_wait_event()` with the lock
  released, on a signal bit of their own, never `SIGF_SINGLE`. A close never
  gives up while the VPU makes progress.
- SoC differences live in `struct vchiq_platform` only.

### videocore.codec (section 6)

- Lock order: `image_lock`, `session_lock`, `mmal_lock`.
- `VCB_Image()` holds `image_lock` throughout and `session_lock` for each
  session call only; it waits for the picture without it. Every other `VCB_*`
  entry point holds `session_lock` throughout.
- Never hold `mmal_lock` across an `mmal_*` call: those take it and may sleep
  with it released. `link_wait()` releases it once, so a path already holding it
  sets a parameter with `port_param_set()`, never `mmal_port_param_set()`.
- `VCB_GetInfo()` and the expunge ask the client (`mmal_info()`,
  `mmal_expunge()`); they never take its lock.
- What the VPU sends (handles, cookies) is compared, never followed.
- `mmal_*` calls return 0 or a `VCERR_*`. Raw MMAL statuses and `LINK_ERR_*`
  stay in the client, the link and the log; `vcerr_from_status()` is the one
  conversion.
- The session core (`videocore_session.c`) reads no kind-specific field: what
  differs goes through `struct session_ops`. `enum output_state` and `enum
  link_state` are one state each, never flags.
- The receive task works under `mmal_lock` only and never waits for a request:
  a bulk receive finding the pool empty parks (`pending_rx`), and no receive
  overtakes a parked one.
- A buffer message and its payload's bulk go out back to back, both requests
  taken from the pool first.
- Receive every bulk the VPU announces, kept or not; a sink takes the unwanted.
- Each session object embeds its `struct mmal_buffer`: free an object only once
  it is detached or its component destroyed.
- `mmal_zero_copy.c` alone decides what is imported: a port's worth
  (`port_worth_importing()`) and a buffer's fit (`zero_copy_qualifies()`, which
  the image calls' budget asks through `mmal_importable()`), all of a port's
  buffers or none. Memory goes back only after the VPU's RELEASED.
- Each session's close gives its large arenas back (`dma_pool_shrink()`), or
  mixed picture sizes pile them up.
- Names: the public calls are `mmal_*`, `smem_*`, `link_*`, `session_*`; internal
  shared helpers are noun_verb; an exported internal never uses a noun the other
  layer exports (`port`, `event`, `format`, `tunnel` are the MMAL client's; `set`,
  `object`, `shape`, `output` the session layer's). Protocol constants are
  `MMAL_*` and `SMEM_MSG_*`, the client's own sizes `CLIENT_*`, the link's
  `LINK_*`.

### videocore.audio (section 7)

- The slave task owns every request and buffer. `AHIsub_Stop()` returns only
  once every chunk is home: no hook runs and nothing is freed while the VPU can
  still read it.
- `AHIsub_Update()` and `AHIsub_HardwareControl()` stay interrupt-safe: store
  and `Signal()`, nothing else.
- `AHIsub_Disable()`/`Enable()` are asm stubs that preserve every register.
  `lib_Version` stays 4, so AHI 4 opens the driver.
- Programs never hang: a failed service makes the stream dead, and the timer
  goes on pacing the hooks.

### vcjpeg.datatype (section 8)

- OM_NEW runs in the caller's task, any number at once. `codec_lock` guards
  `videocodec.library`, the count of pictures on the VPU and the idle task.
- Only a Process opens `videocodec.library`: it scans for backends.
- The expunge never waits: it signals the idle task and refuses while it lives.
- The recognition function runs in whatever task classifies a file: it keeps no
  pointer and allocates nothing.
- Read pixels from another class's object with a direct method call, never
  `DoDTMethodA()`: that overwrites the message's pixel pointer.

## Where things are

Every source file opens with what it does: `head -3` before reading it whole.
Headers live in each binary's `include/`, sources in its `src/`. A binary's
public headers reach the others only through a copy in the build tree
(`public-include/`).

| Path | What |
|---|---|
| `vchiq/include/devices/vchiq.h` | the device's public interface |
| `vchiq/include/vchiq/` | the wire format (`vchiq_protocol.h`, size-asserted), the core's interfaces, the test-only commands (`vchiq_test.h`) |
| `vchiq/src/vchiq_{session,rx,tx,service,bulk}.c` | the transport core, lock-free itself: the caller serialises |
| `vchiq/src/` | `device*.c`, `unit.c`, `unit_task.c` (the one task), `irq.c` (the doorbell server), `vchiq_link.c` (up, down, the reset quiesce) |
| `videocodec/include/libraries/` | `videocodec.h` the public contract, `videocodec_backend.h` the backend ABI; `videocodec/sfd/` the tables |
| `videocore/src/mmal_*.c`, `smem_client.c` | the MMAL client, the `SMEM` client |
| `videocore/src/videocore_link.c` | the link to the device: both services, the receive task |
| `videocore/src/videocore_session.c` | the session core |
| `videocore/src/session_*.c` | one file per kind with its `struct session_ops` (`decoder`, `still`, `encoder`, `isp` the converter); `objects`, `formats` shared |
| `videocore/src/videocore_{backend,codecs,image,gencmd}.c` | the `VCB_*` entry points, the codec table, `VCB_Image()`, the codec heap through gencmd |
| `videocore/include/` | `*_proto.h` the wire formats; `mmal_internal.h` and `session_internal.h` are their layer's own |
| `ahi/` | `audio_lib.c`, `audio_sub.c` (the `AHIsub_*` calls), `audio_slave.c`; `modefile.py` writes the mode file in the build |
| `datatype/` | `vcjpeg_class.c`, `vcjpeg_recog.c` (linked on its own, entry first); `descriptor.py` writes and checks the descriptor in the build |
| `shared/` | `image_info.c` (a still picture's header), `include/auds_proto.h` (the AUDS wire format) |
| `tools/` | `VCInfo.c`, the examples, `VPUPlay.c`; `vcgrab/` the recorder, its muxer the `mkv` submodule (mkv-writer, tested there); `vctest/` one file per command; `common/` the rigs' shared code, never the examples' |
| `tests/` | `conftest.py` the fixtures, `harness/`, `test_*.py` one module per area, `unit/` host-only, `baselines.json`; `.cache/`, `.venv/` and `runs/` are not checked in |
