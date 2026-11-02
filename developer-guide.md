# emu68-vchiq-library: developer guide

For programmers using `videocodec.library` or `vchiq.device`, writing a codec
backend, or working on the component itself. End-user installation is in
[README.md](README.md), how it works and why in
[docs/architecture.md](docs/architecture.md), what the firmware does as measured
in [docs/firmware-facts.md](docs/firmware-facts.md).

Sections 1 to 4 are the programming interfaces. Sections 5 to 7 are building,
the tools and troubleshooting. Section 8 is the hardware test suite.

---

## 1. Which interface to use

| You want | Use | Header |
|---|---|---|
| decode, encode or convert video and pictures | `videocodec.library` | `libraries/videocodec.h` |
| to add a codec implementation (hardware or software) | the backend ABI | `libraries/videocodec_backend.h`, `sfd/videocodec_backend.sfd` |
| another VideoCore firmware service (gencmd, audio, ...) | `vchiq.device` | `devices/vchiq.h` |

The build installs the headers under `include/`, with the generated `proto`,
`clib`, `inline` and `pragmas`; the stack's archive has them in
`Developer/include`.

---

## 2. videocodec.library

The header's opening comment is the contract: four worked examples, the
ownership rules, and every tag grouped by who reads it. This section is the
overview. `tools/VCPlay.c` is a complete player of about 400 lines to copy
from, `tools/VCShow.c` a picture viewer on the image calls (6.2).

### 2.1 Sessions

A session is opened with a tag list and driven by polling:

```c
struct VCSession *s = VC_OpenSessionTags(VCA_Codec, VCC_H264,
                                         VCA_Kind, VCK_Decoder,
                                         VCA_OutputFormat, VCF_RGB16,
                                         VCA_SigMask, 1UL << sig,
                                         VCA_ErrorPtr, (ULONG)&err,
                                         TAG_DONE);
for (;;)
{
    struct VCItem it;
    while (VC_Poll(s, &it))
        switch (it.Type)
        {
        case VCIT_PacketReturned:
            /* fill it.u.Packet from the file */
            VC_SendPacket(s, it.u.Packet);
            break;
        case VCIT_Frame:
            /* show it.u.Frame */
            VC_ReleaseFrame(s, it.u.Frame);
            break;
        case VCIT_Event:
            /* VCEV_StreamInfo, VCEV_EndOfStream, VCEV_Error */
            break;
        }
    Wait((1UL << sig) | SIGBREAKF_CTRL_C);
}
VC_CloseSession(s);
```

| Kind | Input | Output | Codecs |
|---|---|---|---|
| `VCK_Decoder` | packets | frames | `VCC_H264`, `VCC_MJPEG` (still pictures are image calls: 2.4) |
| `VCK_Encoder` | frames | packets | `VCC_H264`, `VCC_JPEG` |
| `VCK_Converter` | frames | frames | `VCC_Raw` |

An encoder or converter reads the formats of its `VCCodecInfo.InputFormats`
and refuses others at the open with `VCERR_UNSUPPORTED`; the converter's are the
ones it writes and a camera's `VCF_YUYV` and `VCF_NV12`. `OutputFormats` says
what a row writes: 0 for an encoder. NV12 goes as a camera sends it, the UV rows right
after the picture's own; I420 keeps whole 16 rows. `VC_ConvertImage()` takes the
same. The VideoCore reads YUV as full-range BT.601 whatever it is told, so a
camera's video-range picture (16 to 235) comes out with its blacks at 16 and its
whites at 235: a little flat, nothing lost.

* **Objects.** Each side is a fixed set of `struct VCPacket` or `struct VCFrame`
  objects: the library's own (sized by `VCA_InputCount`/`InputSize`/
  `OutputCount`/`OutputSize`) or your memory, imported with
  `VC_ImportPacket()` / `VC_ImportFrame()` before the first send. Input
  objects come to you empty as `VCIT_PacketReturned`/`VCIT_FrameReturned`,
  you fill and send them; output objects come full as `VCIT_Frame`/
  `VCIT_Packet`, you use and release them. Nothing is copied.
* **Order.** `VCEV_StreamInfo` precedes the first frame of a new shape. A
  decoder opened with an output format but no size waits after that event
  until your next poll, so you can pick a screen mode, call
  `VC_SetAttrsTags()` with the geometry and import your frames.
* **A video that changes shape partway.** A decoder's native I420 frames
  follow it: `VCEV_StreamInfo` again, then frames of the new size. Through
  the hardware scaler (any other output format, or a size) the frames keep
  their size and the scaler follows the change by itself, with no event;
  with `VCA_KeepAspect` the picture keeps the place the first shape's aspect
  gave it.
* **Changing a running session.** `VC_SetAttrsTags()` takes the output
  geometry tags, `VCA_Bitrate` and `VCA_RequestKeyFrame`. A geometry change
  replaces that side's objects: old pointers are invalid, re-import yours.
* **Seeking.** `VC_Flush()` drops everything the session holds: the input
  packets come back unused, and no frame from before the flush reaches you
  after it, those inside the hardware's pipeline included. Then send the
  stream again from a key frame.
* **Errors** are negative `VCERR_*`. `VCERR_DEVMEM` is the hardware's own
  memory (the VPU's `gpu_mem`), `VCERR_NOMEM` Amiga memory, `VCERR_NOBACKEND`
  no backend for the codec and kind.
* **Growth.** Every structure starts with `Size` and may grow at its end:
  set `Size` before calls that fill one, read only up to `Size` after
  (`VC_HAS_FIELD(info.Size, struct VCInfo, MemFree)` says whether a field is
  there).

### 2.2 Pictures into and out of a screen bitmap

Import the bitmap's memory as output frames and the ISP writes the picture by
DMA, with no 68k copy (header example 2, `tools/VPUPlay.c`). Import it as a
converter's input frame and the ISP reads the screen the same way
(`tools/vcgrab/record.c`). Requirements the VideoCore backend checks:

* the pixel format is one it handles: `VCF_RGB16` (`RGBFB_R5G6B5PC`), `VCF_RGB24`,
  `VCF_BGR24`, `VCF_BGRA32`, `VCF_RGBA32`; never ARGB;
* rows are a whole number of 32-pixel groups (1920, 1280, 1024, 800, 640 are;
  1366 is not) and heights are even;
* the memory stays at one address while imported (lock the bitmap to read its
  address; a displayed bitmap does not move);
* the address is on 4 bytes; VRAM needs no cache maintenance;
* only the bitmap on display is in VRAM: a hidden one holds another screen's
  picture there, so watch that the screen stays in front.

Memory to share with the VPU must lie within its reach, which plain Fast RAM
need not on a machine of more than 1 GiB. To pass pictures between two sessions
without a copy, use one session's own frames, which do, and import their memory
into the other. A `VCFrame` itself belongs to one session, so each picture goes
by two objects; import before the first send, and close the importing session
first.

To encode RGB, or at another size, no second session is needed. An H.264
encoder takes every format of its `VCCodecInfo.InputFormats`, and scales to
`VCA_OutputWidth` and `VCA_OutputHeight`: the backend puts the ISP in front of
the encoder inside the VPU, and `VCGrab` sends the screen's own memory, imported
as the session's one input frame. That frame stays out while the encoder is
behind, so a program that drops a picture when it finds the frame out counts
what was really lost. `VCFF_NoPicture` with `VCFF_EndOfStream` sends the end
mark alone, for a program whose memory may no longer hold a picture. Such a
session has no flush, and `VCA_DestRect`, `VCA_KeepAspect` and `VCA_OutputPitch`
are refused.

The ISP makes full-range BT.601 of RGB, whatever it is told, and the session
writes that into the stream by itself. For I420 in, `VCA_ColorSpace` says what
the pictures hold.

### 2.3 What the decoders take

**H.264** streams must be Annex-B and 8-bit 4:2:0 (Baseline, Main or High
profile), at most 2032 pixels wide and 8192 macroblocks: 1920x1080 and
1080x1920 fit, 1920x1200 and 2048x1080 do not. The level the stream declares
plays no part. Past those limits the firmware gives wrong pictures, or none,
without a word, so videocore.codec checks: a profile past High fails the
session with `VCERR_STREAM` at its SPS, a larger picture as soon as its size
comes.

**MJPEG** pictures are one baseline JPEG per packet, at most 4096 pixels wide
and 1920 tall. Past those the firmware gives a grey picture or none, again
without a word, so `VC_SendPacket()` answers `VCERR_STREAM` and the packet stays
yours.

`MaxLevel` (4.2) is the highest level it keeps up with: a level 4.2 stream at
its ceiling (1080p60, 64 Mbit/s) decodes at 67 fps, a level 5.0 one
(186 Mbit/s) at 31.

**The SPS and PPS** come in the stream or beside it: `VCA_Extradata` and
`VCA_ExtradataSize` pass them in Annex-B form, any size, read during the open
only. Pass them where you have them: the stream's memory needs are then
checked at the open, and a player can fall back to its own decoder before
showing anything. Without them the check comes at the first packet (both
`VCERR_DEVMEM`). A decoder the firmware has no memory for gives no picture and
says nothing, so videocore.codec also watches for that: one that has taken
4 MB without a picture size fails with `VCERR_DEVMEM` when the codec heap is
nearly full, and any after 16 MB with `VCERR_STREAM`.

**MJPEG** goes one whole picture per packet (the video decoder joins no
pieces), with the end mark on the last picture's packet: an empty end mark
decodes nothing.

### 2.4 Image calls and still pictures

Three calls each do one picture, blocking, over your buffers (`VCA_Data`,
`VCA_Dest`):

| Call | From | To |
|---|---|---|
| `VC_DecodeImageTags()` | a still picture: JPEG, PNG, GIF, BMP, TGA, PPM | a picture of a given size and format |
| `VC_EncodeImageTags()` | a picture | a JPEG; returns the bytes written |
| `VC_ConvertImageTags()` | a picture | another size and format |

Still pictures are `VC_DecodeImageTags()`'s only: no session decodes them. A
JPEG through the video decoder (`VCC_MJPEG`) is a stream's: a decoder session,
one whole picture per packet (2.3).

Calls from several tasks are safe: videocore.codec queues them, one picture on
the VPU at a time. A call waits at most `VCA_Timeout` milliseconds for each
pass (videocore.codec: 5000 by default), and its timeout starts at its turn.

**Reading the header.** `VC_GetImageInfo(data, length, &info)` reads a still
picture's header without decoding, to size the destination: its codec (from
the first bytes; set `info.Codec` to `VCC_TGA` for a TGA, which has no
signature), size, transparency (`VCTR_*`: none, a mask, alpha levels) and a
GIF's frame count.

**Decoding.** `VC_DecodeImageTags()` tells the codec from the bytes when
`VCA_Codec` is not given (`VCERR_STREAM` at once if they name no format; a TGA
needs `VCC_TGA`), and takes the picture's own size when no output size is
given. A picture whose data is cut short (a JPEG without its end marker, a PNG
without `IEND`, a GIF's first image unfinished, fewer bytes than a BMP's,
TGA's or PPM's header implies) is refused with `VCERR_STREAM` before the
hardware sees it. So is anything the hardware gets wrong
(`VCERR_UNSUPPORTED`):

| Format | Decoded | Refused |
|---|---|---|
| JPEG | baseline and extended, 4:2:0, 4:2:2, 4:4:4, grey, restart markers | progressive, CMYK, lossless, hierarchical, arithmetic, 12-bit; a header past 80 KiB without its longest run of metadata |
| PNG | 8-bit RGB, RGBA, palette (with transparent entries or alpha levels); heights of whole 16 rows | grey, grey with alpha, 16-bit, interlaced, other heights (the firmware loses rows near the bottom), a transparent colour on RGB (it comes out opaque) |
| GIF | the first image, interlaced or not, colours at RGB565 precision | a transparent colour in the first image (it comes out opaque) |
| BMP | 8, 24 and 32 bits uncompressed, either row order (32: the fourth byte is alpha) | RLE, bit fields, OS/2 |
| TGA | 24 and 32 bits uncompressed (`VCA_Codec` `VCC_TGA`) | RLE (it takes the firmware down), palette, grey |
| PPM | P6 and P5, 8 bits | 16 bits |

**One pass or two.** The picture goes straight into your buffer when that is
the picture's own size in I420, RGB16 or RGBA32, with a width of whole 32
pixels, a height of whole 16 rows and the natural pitch. Anything else (RGB24,
BGR24, BGRA32, another size, `VCA_DestRect`, `VCA_KeepAspect`, another pitch
or width) takes a second pass through a converter, and that has two costs:

* it needs the VPU memory of both pictures at once, so a very large picture
  can be refused with `VCERR_DEVMEM` while it decodes straight;
* the converter writes alpha 255, so transparency survives only in RGBA32
  straight from the decoder. A program that needs it at another size scales
  it itself, and one whose picture's width is no whole 32 pixels keeps its own
  decoder for it.

**Size and memory.** The VPU works in your buffer itself if it can reach it:
Pi RAM in the lowest GiB or the VRAM (never Chip RAM), and for an output on
whole 64-byte cache lines unless in the VRAM. Otherwise the picture goes by
bulk, with a copy in the VPU's memory. A picture past these limits is refused
from its header:

* One bulk transfer carries 126 MB, an RGBA picture of about 31 megapixels
  (`VCERR_UNSUPPORTED`). Pictures the VPU works in have no such limit:
  8192x6144 to RGB24 (151 MB) decoded.
* The codec heap has to hold the decoder's picture (1.5 bytes per pixel for a
  JPEG), a 16 MiB margin and a copy of every picture that goes by bulk
  (`VCERR_DEVMEM`). At the default `gpu_mem=76`, 51 MiB free, a JPEG takes up
  to 24 megapixels (5696x4272 measured in I420).

A packed picture the VPU cannot write itself goes through the backend's own
frame and one 68k copy; with `VCA_KeepAspect` it goes by bulk, as planar ones
always do. Image calls keep their components between pictures, for speed, and
let them go when a picture would not fit.

### 2.5 Backends and codec memory

`VC_QueryCodec(index, &info)` lists every backend's codec rows (index 0
upward until it fails): codec, kind, size limit, `MaxLevel`, pixel formats
(`OutputFormats`, a mask: `info.OutputFormats & VCF_MASK(VCF_RGB16)`), the backend's
name, the row's priority and what it serves (`Flags`: `VCCF_Session`,
`VCCF_Image`).

A session goes to the row of the highest priority for its codec and kind that
serves sessions, an image call to one that serves image calls. Pass
`VCA_Backend` with a row's `Backend` name (with or without `.codec`) to pick
one yourself. No row, no backend: `VCERR_NOBACKEND` (a session for a still
picture's codec, say).

`VC_GetBackendInfo(index, &info)` reports each backend's state: its sessions,
codecs and `MemTotal`/`MemFree`, the hardware's own memory its codecs allocate
from. On the Pi that is the codec heap of `gpu_mem`: a 1080p decoder with its
ISP takes about 30 MB, 68 MB with 16 reference frames. Each call costs
videocore.codec a few milliseconds of gencmd.

`Interface` and the counters are the backend's own, for diagnostics: point
`Counters` at room for `MaxCounters` of them, and `NumCounters` says how many
the backend has (more than were written: the room was short). videocore.codec
reports `MMAL x.y` while its service is open, and counts `events`,
`events_dropped`, `buffers_back` and `stray` (the last two should stay 0).

---

## 3. Writing a backend

A backend is an Exec library named `<name>.codec`, installed in
`LIBS:videocodec/`. It implements the `VCB_*` function table of
`sfd/videocodec_backend.sfd` (arguments in registers, base in A6); the
header's opening comment states the contract, and architecture.md 5.2 how
backends are found and chosen. `videocore/` is the reference implementation.

* `VCB_GetInfo()` is called first and must report `VCB_ABI_VERSION` and
  `VCInfo.Codecs`, the number of rows `VCB_QueryCodec()` answers. A backend
  of another ABI major is refused.
* Every codec row carries a `Priority`: `VCB_PRIORITY_HARDWARE` (50) for
  dedicated hardware, `VCB_PRIORITY_SOFTWARE` (0) for the 68k doing the work;
  and `Flags`: whether it serves sessions (`VCCF_Session`), image calls
  (`VCCF_Image`) or both.
* `VCB_OpenSession()` receives a `struct VCSessionParams`, the frontend's
  parse of the client's tags; a backend never sees a tag. `Changed` marks
  what a `VCB_SetAttrs()` carries.
* `VCB_Image()` is one image call: a `struct VCImageParams` with
  its kind, codec and both sides (compressed bytes or a picture, in the
  caller's memory), blocking in the caller's task. How the picture is made
  is the backend's; it fills `Written` for an encoder and must not keep its
  other callers out while it waits.
* `VCB_Idle()` comes when the frontend's last opener closes it, in
  that opener's task: let go of what you keep between calls for speed (a
  connection, components set up); the next call makes it again. You may wait
  for your hardware there. An expunge with such things kept is refused until
  then.
* The backend allocates the `VCFrame`/`VCPacket` objects (public part first,
  its own data behind), hands them out through `VCB_Poll()`, and frees them
  with the session. Ownership, order and signalling are as the public header
  promises the client.
* Every structure is `Size`-prefixed and grows only at its end; read only up
  to the smaller of your `sizeof` and the peer's `Size`. Functions are added
  only at the end of the table.
* Any task may call a backend at any time; it serialises itself.

---

## 4. vchiq.device

One `OpenDevice()` opens one VCHIQ service, named by fourcc, and waits for the
VPU's answer. Up to 16 services and any number of openers work at once; a task
shares a service by copying the opened request. Everything else is Exec I/O,
replied to the request's port. `iovq_FourCC` 0 opens no service, for the
information commands only.

| Command | Does |
|---|---|
| `CMD_READ` | the next message of the service, copied into `io_Data` (`io_Actual` its size) |
| `CMD_WRITE` | one message to the VPU (at most 4088 bytes) |
| `NSCMD_VQ_BULKWRITE` / `NSCMD_VQ_BULKREAD` | a buffer to or from the VPU by DMA, any alignment; replied when it has moved |
| `NSCMD_VQ_GETINFO` | the connection's counters and how it came up at this boot (`struct VchiqInfo`, `vi_Bringup`: fresh, resumed, repaired, re-initialised); where the VPU reaches host memory and at which bus address (`vi_ReachStart`, `vi_ReachEnd`, `vi_ReachBus`: what the `SMEM` service imports) and the VRAM, which needs no cache maintenance (`vi_UncachedStart`, `vi_UncachedEnd`) |
| `CMD_FLUSH` | every queued request of the opener's service back, `IOERR_ABORTED` |
| `NSCMD_DEVICEQUERY` | the NSD query |

A gencmd exchange:

```c
struct IOVchiqReq *io = (struct IOVchiqReq *)CreateIORequest(port, sizeof(*io));
io->iovq_FourCC = VCHIQ_FOURCC('G', 'C', 'M', 'D');
io->iovq_Version = io->iovq_VersionMin = 1;
if (OpenDevice((CONST_STRPTR)VCHIQ_NAME, 0, (struct IORequest *)io, 0) == 0)
{
    /* a copy of io with CMD_READ and a buffer, SendIO()d; then: */
    io->iovq_Req.io_Command = CMD_WRITE;
    io->iovq_Req.io_Data = "version";
    io->iovq_Req.io_Length = 8;
    DoIO((struct IORequest *)io);
    /* WaitIO() the read: the reply is in its buffer */
}
```

* **Order.** Writes and bulk transmits of one service go out in the order they
  were issued, so a message and the bulk transmit that belongs to it stay
  together. Bulk receives keep their own order and never wait behind a
  transmit: the VPU allows four bulks in flight per direction, and may have to
  finish a receive before it takes the next transmit.
* **Keep reads posted.** A service holds at most 128 unread messages, then the
  device stops reading the VPU's stream until its client reads.
* **The connection** comes up at the first open and stays up across closes and
  Amiga resets, which a client need not know about (architecture.md 3.2).
  `Avail FLUSH` gives the device's memory block back, contents intact.

---

## 5. Building

From the stack root, inside the container as always:

```sh
./scripts/docker-build.sh --target emu68-vchiq-library
```

or `./build.sh --build` for the whole stack. The whole component has one
version, set in its root `CMakeLists.txt`. Outputs under `install/`:

| File | What |
|---|---|
| `DEVS/vchiq.device` | the transport |
| `LIBS/videocodec.library` | the API |
| `LIBS/videocodec/videocore.codec` | the VideoCore backend |
| `DEVS/AHI/videocore.audio`, `DEVS/AudioModes/VIDEOCORE` | the AHI driver and its modes |
| `Classes/DataTypes/vcjpeg.datatype`, `Storage/DataTypes/VCJPEG` | the JPEG datatype and its descriptor |
| `C/VCInfo`, `Tools/Commodities/VCGrab` and `.info` | the user's tools |
| `Developer/Tools/VCTest`, `Developer/Tools/VPUPlay` | the test rig and the test player |
| `Developer/Examples/videocodec/` | the examples: `VCPlay`, `VCShow`, their sources, `ReadMe` |
| `include/libraries/videocodec.h`, `videocodec_backend.h`, `include/devices/vchiq.h` and the generated headers | the SDK |

`./build.sh --upload` sends `DEVS`, `LIBS` and `C` with the rest, not
`Developer/`, `Classes/` or `Storage/`: the test suite uploads those itself.
The stack's `.lha` carries everything, and its Install script does what
README.md, Installation, says. `Developer/` stays in the archive. The test
suite has a build flavour of its own (8.2).

---

## 6. Tools

### 6.1 VCInfo

The user's check, installed to `C:`. One command per run, each ending with
`PASS` or `FAIL`; return codes 0 PASS, 5 PASS with warnings, 10 a part not
installed, 20 failed.

| Command | Does |
|---|---|
| (none) | the setup in plain words, each part OK or a warning or failure with its cure |
| `CODECS` | every backend's codec rows with their priority and what they serve |
| `GCMD command` | one gencmd command through the device and its answer, as vcgencmd gives it; quote a command of several words (`GCMD "measure_clock arm"`). A missing reply dumps the shared window. |
| `COMPONENTS` | which MMAL components the firmware has, and gencmd's `codec_enabled` for each codec |
| `COUNTERS` | the connection's state and counters, where the VPU reaches host memory (`reach:`), and every backend's |
| `DUMP` | the shared window's state, read-only, for a bug report |

Without a command it goes through Emu68's resources and the VCHIQ node (read
without the device), `cma.resource`, the device and how its connection came
up, the firmware's version, temperature, throttling and under-voltage, clocks
and `gpu_mem`, the library, its codec drivers and their codec memory. With
`vcjpeg.datatype` installed it also classifies the header of a large baseline
JPEG: anything but `VCJPEG` is a warning with the `S:User-Startup` line as its
cure.

`COMPONENTS` opens a `"mmal"` service of its own. The firmware lists no
components, so a fixed set of names is tried; each one found is created, its
ports read (format, buffer needs, `SUPPORTED_ENCODINGS`) and destroyed at once,
never enabled.

### 6.2 VCPlay and VCShow

The examples to copy, in the archive's `Developer/Examples/videocodec` as
source and binary, with a `ReadMe` (`tools/examples.readme`) giving the
one-line build. Each takes a file name, opens a window on the Workbench and
uses nothing of the stack but the library's headers.

`VCPlay file` is the example of sessions. `VCShow file` is the example of image
calls: `VC_GetImageInfo()` for the size, a window of that size (scaled down to
fit the screen), one `VC_DecodeImageTags()` into a buffer in the screen's pixel
format, then `p96WritePixelArray()`. A picture the hardware refuses
(`VCERR_UNSUPPORTED`) ends with a message and return code 5: the place where a
viewer falls back to its own decoder.

### 6.3 VCTest

The rig the test suite drives (8), in the archive's `Developer/Tools`, not
installed. The first word picks the command, the rest is that command's own
ReadArgs template. `VCTest ?` lists the commands and `VCTest DECODE ?` shows a
command's template; the notes at the top of `tools/vctest/vctest.c` and of each
command's source say what the arguments do.

| Command | Does | Facts |
|---|---|---|
| `STATUS` | vchiq.device's connection, the Pi's timer and every backend's state and counters | `vchiq.*`, `timer.now`, `backend.N.*` |
| `CODECS` | the codec table | `codec.N.*` |
| `MARK text` | a line in the debug log, the capture's self-test | |
| `ABANDON` | leaves the session as a crash in mid-flight would, for the repair test; needs the test hooks (8.2) | `abandon.*` |
| `AUDIO` | silence or a tone through the firmware's audio service, driven directly; `DEST` 1 the jack, 2 HDMI0, 3 HDMI1 | `run.N.*`, `stop.N.*`, `pins.*` |
| `AHIWALK`, `AHITONE` | a channel walk or a tone through an AHI mode | `ahi.*` |
| `DECODE` | a decoder session over an Annex-B file, each frame's MD5 compared with ffmpeg's `framemd5` | `decode.*`, `stream.N.*`, `ref.*`, `mismatch.N.*`, `flush.*` |
| `IMAGE` | a still picture through `VC_DecodeImage()` | `header.*`, `image.*` |
| `TOJPEG`, `CONVERT` | the other image calls | `encode_image.*`, `convert.*` |
| `ENCODE` | an H.264 encoder session over a raw file, I420 or any `FORMAT`, `OUTWIDTH` and `OUTHEIGHT` through the ISP; `PACE`, `PACKET`, `GAPS`, `ENDEMPTY`, `HOLD` for the tests | `encode.*`, `packet.N.*` |
| `DATATYPE` | a picture through datatypes.library, as a program loads one | `datatype.*` |
| `PAGE` | many pictures at once, a process each (64 at most), as a browser loads a page | `picture.N.*`, `page.*` |
| `HOLD` | the free RAM the VPU reaches taken, all but `LEAVE` MiB at the end of the largest chunk, until Ctrl-C | `hold.*` |
| `SCREEN` | a Picasso96 screen to record: the colour bars and a moving block, or `NOISE`; `BACK n` steps it behind the others after n seconds, for three; `NATIVE` or `DEPTH 8` a screen VCGrab must refuse | `screen.*` |

`DECODE`'s `OUT` writes the frames raw, the input of `TOJPEG`, `CONVERT` and
`ENCODE`; `PACKET n` sets the size of an input packet, and from 512 KiB on the
backend imports them (`SMEM`). `MISALIGN` puts the image calls' pictures 4
bytes past a cache line, so the VPU cannot write them. `BACKEND name` runs any
of the codec commands on that backend.

**Output.** The grammar of `tools/common/kv.h`: `format=1` first, then one fact
per line as `key=value` (indexed groups by number: `stream.0.width`), prose for
people indented two spaces, and `PASS` or `FAIL` last. Every run writes
`[VCTest] begin ...` and `[VCTest] end rc=N` to the debug log, by which the
suite slices the log.

| Return code | Means |
|---|---|
| 0 | the command did its work and its own checks passed |
| 5 | a check failed (`fail=` says which) |
| 10 | the environment: a library, device, file or memory missing, or no test hooks |
| 20 | the API refused or failed (`error=`), nothing happened for five seconds, or the arguments made no sense (then no `format=` line comes out at all) |

### 6.4 VPUPlay

```
VPUPlay file [FPS n] [FAST] [FRAMES n] [DEPTH 16|24|32] [WIDTH n] [HEIGHT n] [VERBOSE] [WINDOW] [LOOP] [WINDOWTEST]
        [TRACE]
```

The measurement rig of the display path, beside `VCTest` in `Developer/Tools`.
It reads H.264 elementary streams only; users play video with MintVID (README,
Playing a video).

Full screen it opens a Picasso96 screen three picture heights tall, imports the
session's output over its parts, and scrolls the viewport to each finished
picture from a helper process. The mode is Picasso96's best fit for the picture
(or `WIDTH`/`HEIGHT`) at `DEPTH`, by default the Workbench's. `WINDOW` plays in
a resizable window on the Workbench instead, the backend's own frames copied
in.

`LOOP` restarts the file without an end mark, so the decoder never stops.
`FAST` ignores the frame rate, `FPS` overrides it (default: the stream's, else
30). `FRAMES n` stops after n pictures; any key, a mouse click or Ctrl-C stops
it at once. `WINDOWTEST` makes the window move, grow and shrink by itself, as
a user's drags would. `TRACE` echoes every line to the debug log, where a hang
can be read when the screen is in front.

Its facts come out in VCTest's grammar (`stream.N.*`, `screen.*`, `output.N.*`,
`window.*`, `play.*`). Return code 5 means it stopped before a picture.

---

### 6.5 VCGrab

The user's recorder (README, Recording the screen), in
`SYS:Tools/Commodities/`, written up in `tools/vcgrab/`. `screen.c` finds the
screen and says where it is at each tick, `record.c` runs the encoder session,
the `mkv` submodule writes the file, `still.c` is the two image calls, `settings.c` what is
asked for and `ENV:VCGrab.prefs`, `window.c` the ReAction window, `commodity.c`
the broker and the loop around them, and `vcgrab.c` the arguments, the Workbench
start and what is said to the user. It prints prose and ends with a report line
the tests read; it does not speak the rigs' grammar.

All of it is one task, the commodity's hot keys and window served between a
recording's passes, so nothing there may block while one runs: `window.c` holds
the drawer requester back during a recording, and failures go to the status
line and `DisplayBeep()`, not a requester. Under Workbench nothing may be
written to `Output()`: libnix selects a console that would open at the first
write and wait to be closed.

`dist/VCGrab.info` is generated: `dist/icons/` holds its art, and
`dist/icons/make_icons.py` builds it.

The muxer is `rondoval/mkv-writer`, shared with Neptune and tested in its own
repository, on the host with the sanitizers on, against its own EBML reader and
ffmpeg; `test_vcgrab.py` borrows that reader from the submodule. mkvalidator
reports `Missing element 'SeekID'` on every file without CRC-32 elements,
ffmpeg's own included: its parser's fault, CRC-32 being optional.

---

## 7. Troubleshooting

* **No frame ever comes** (`nothing happened for 5000 ms`): the stream is not
  Annex-B (an MP4 file, say). The counters printed say whether the input was
  consumed and whether the output returned anything.
* **`VCERR_STREAM` (-8) as the stream starts**: an H.264 profile past High
  (10-bit, 4:2:2, 4:4:4), or a picture over 2032 pixels wide or 8192
  macroblocks; the debug log names the size.
* **A still picture refused (`VCERR_UNSUPPORTED`, -6)**: a coding the
  hardware gets wrong or dies on (2.4), or a picture past one transfer; the
  debug log prints its header facts. Decode it in software.
* **`VCERR_DEVMEM` (-2)** when a session opens or starts, or status 1
  (ENOMEM) in the log at a component's creation or the ISP's enables, or
  `codec heap ... free` in the log of a decoder without a picture: raise
  `gpu_mem` in config.txt. `VCInfo` shows the codec memory.
* **`no picture size from the decoder`** (VPUPlay, after 5 s): the stream is
  not one the decoder takes. A decoder out of memory shows as `VCERR_DEVMEM`
  instead (2.3).
* **Frames but mismatches**: write a few with `OUT` and compare with
  `ffmpeg -i stream.h264 -frames:v 3 -f rawvideo ref.yuv` to tell geometry
  (crop, pitch) from pixels.
* **Wrong colours on screen**: the Picasso96 format is paired with the wrong
  `VCF_*` in the player's format table (`tools/VPUPlay.c`, `formats[]`).
* **`no RTG screen mode ... in a format the backend writes`**: add a mode of
  that size and depth in Picasso96Mode, in one of the formats the message
  names.
* **The three-part screen does not open** (VPUPlay's `screen.parts=2`): not
  enough VRAM for three picture heights (1080p: 12.4 MiB at 16 bits, 18.7 at
  24, 24.9 at 32, plus the Workbench's screen). With two parts full screen
  plays at about 17 fps with nearly every frame late; with one it tears. Give
  the RTG card more video memory: `dtoverlay=videocore,mem=N` in config.txt
  (MiB, default 16; 32 for 1080p at 24 bits). Emu68 1.1 ignores the older
  `vc4.mem=` in cmdline.txt; its boot log names the size
  (`[VC] Memory base at ..., size NMB`).

---

## 8. Hardware tests

The regression suite in `tests/` runs on a Linux host and drives one Amiga
through amimcp's Python client. It replaces the installed files (README,
Installation) with the install tree's and reboots, uploads the rigs and the
test material to `RAM:hw`, runs the tests, captures the debug log from the
serial port, and writes a run directory.

pytest is the runner. A test is a function and its inputs come from fixtures,
so nothing depends on another test having run. Every outcome is PASS, FAIL or
SKIP with its reason; a harness defect is an ERROR, not a failure. The rigs
print facts and the host judges them (6.3).

### 8.1 Running

**What it needs:**

* on the host: ffmpeg and ffprobe, the stack built in the suite's flavour (8.2)
  with `install/` and `build/` in place, and the venv below;
* on the Amiga: the project installed by the installer (the datatype's line in
  `S:User-Startup` included), `gpu_mem=192` and `videocore,mem=32` in
  config.txt, and RTG modes of the picture's size at the Workbench's depth and
  at 32 and 24 bits;
* something on HDMI0 and the jack for the audio tests;
* the machine's serial adapter on `/dev/ttyUSB0` for the tests that read the
  debug log.

Once, the venv (Pillow is not on the host; the suite installs nothing):

```sh
python3 -m venv tests/.venv && tests/.venv/bin/pip install -r tests/requirements.txt
tests/.venv/bin/python tests/harness/material.py      # the material ahead of time (optional, ten minutes)
```

A run, from the component root:

```sh
tests/.venv/bin/pytest tests -m smoke                   # one of every binary and path, about six minutes
tests/.venv/bin/pytest tests                            # everything automatic, about 25 minutes
tests/.venv/bin/pytest tests -m "not slow"              # without the level probes and the whole still set
tests/.venv/bin/pytest tests/test_images.py -k jpeg     # pytest's own selection
tests/.venv/bin/pytest tests --collect-only -q          # the list of tests
tests/.venv/bin/pytest tests/unit                       # the host-only tests of the harness itself
```

The options, beside pytest's own (`tests/conftest.py`):

| Option | Does |
|---|---|
| `--server amiga-pi4\|amiga-cm4` | the machine: its amimcp entry in `~/.claude.json`, or `AMIGA_HOST` and `AMIGA_TOKEN`; also the baselines' key |
| `--amimcp path` | the amimcp checkout, by default beside the stack's |
| `--install-dir`, `--build-dir` | the stack's; the build dir's `CMakeCache.txt` says the flavour |
| `--run-dir` | where the run goes (default `tests/runs/<timestamp>`) |
| `--no-install` | check the installed files against the install tree instead of replacing them |
| `--no-reboot` | deselect the tests that reboot |
| `--listen`, `--hands`, `--profile` | select those tiers (8.2) |
| `--serial /dev/ttyUSB0\|none` | the debug log's port |
| `--require-debug-log` | fail, not skip, without a capture |
| `--update-baselines` | write this machine's measured values (8.5) |
| `--keep-ram` | leave `RAM:hw` on the Amiga at the end |

At the start the suite runs `VCTest MARK` and looks for the mark in the log: a
port that opens but shows no mark (another machine's adapter) counts as no
capture, and the tests that read the log skip. The screen tests nudge the mouse
first, which closes the Workbench's screen blanker.

### 8.2 Flavours and markers

The suite's build is the debug flavour with the device's test hooks:

```sh
EMU68_CONFIGURE_ARGS="-DEMU68_DEBUG_BACKEND=pistorm -DEMU68_TIER=off \
    -DEMU68_DEBUG=emu68-vchiq-library -DEMU68_VCHIQ_TEST_HOOKS=ON" \
    ./scripts/docker-build.sh --target emu68-vchiq-library
```

`EMU68_VCHIQ_TEST_HOOKS` compiles `NSCMD_VQ_TEST_ABANDON` into `vchiq.device`:
the next reset leaves the session as a crash would. Any other build answers it
`IOERR_NOCMD`, and no release sets it. What the installed build enables is read
from `CMakeCache.txt`:

| Marker | Meaning |
|---|---|
| `smoke` | one of every binary and path |
| `slow` | minutes on its own: the level probes, the whole still set, the shape change on screen |
| `reboot` | the test or its module reboots (`C:Reboot`); `--no-reboot` deselects them; they run last |
| `hooks` | needs `EMU68_VCHIQ_TEST_HOOKS` in the build; skipped otherwise |
| `profile` | needs the component at the profile tier (8.6); selected only with `--profile` |
| `listen` | someone at the machine says what they heard; selected only with `--listen` |
| `hands` | needs someone's hands (an external reset); selected only with `--hands` |

`--listen` runs what is judged by ear (the tones, the volume step, the channel
walks): each test plays, then asks in the terminal; `n` fails it, `s` skips it.
`--hands` runs the external resets: each test asks for Ctrl-Amiga-Amiga and
waits for the agent to go and come back, once on a clean record and once on a
record left dirty on purpose, which the device must recognise as the VPU's
restart and start afresh.

A test that reads the drivers' lines asks for the `driver_log` fixture and
skips when there is no capture or the build does not log; the first open after
the install reboot must log `[vchiq] up:` on a debug build, or the run stops.

### 8.3 What the modules cover

| Module | What |
|---|---|
| `test_vcinfo.py` | VCInfo as a product: the summary, DUMP, GCMD, COMPONENTS, CODECS |
| `test_device.py` | the connection up, the codec table, a bulk transfer to the audio service, resume after an expunge |
| `test_decoder.py` | bit-exact decodes, the backend named or refused, the decoder's rate, two at once, a JPEG through the video decoder, flush and replay, the shape change, the pixel aspect |
| `test_players.py` | VPUPlay full screen and in a window, the colour bars, LOOP, two players, an empty stream, the shape change and pixel aspect on screen; VCPlay |
| `test_images.py` | the image calls: decode, convert, encode; every still format, refusals, cut and big pictures, the headers; VCShow; a camera's YUYV and NV12 through the converter, a converter session's rate, an input format it refuses |
| `test_encoder.py` | thirty pictures with key frames, a key frame on request; RGB through the ISP: each format, scaled, gaps in the times, the end mark alone, a refused bit rate, closing while stalled, noise |
| `test_vcgrab.py` | VCGrab judged from its files: the bars as a player shows them, every pixel format through the ISP, scaling, refusals, a minute of 1080p, noise at the highest bit rate, stepping back, Ctrl-C at any moment, stills, the hot keys, playback through the VPU. With `VCGRAB_DISK=Stuff:` also the writing to that disk and a recording past 2 GiB and the stop near 4 GiB; with `VCGRAB_WRAP=1` a take across the Pi's timer wrap, up to 72 minutes' wait. Also a full disk, a screen closing, screens it must refuse, and takes beside a player, a decode, sound and a page of JPEGs |
| `test_levels.py` | the H.264 limit probes: declared levels, reference frames, rates, sizes, profiles; MJPEG sizes against ffmpeg |
| `test_audio.py` | the audio service directly: rates, chunk and depth, STOP, two streams, outputs, channel counts; by ear the tone, the volume step, the channel walk |
| `test_ahi.py` | videocore.audio through AHI: the three modes, the driver's log lines, two streams, a break, the services closed; by ear the 7.1 order |
| `test_datatype.py` | vcjpeg.datatype as installed in SYS: (the install step replaced it): routing, pixels, the hand-over, the idle close, twenty pictures at once, a page of 24 MP JPEGs past the RAM the VPU reaches |
| `test_zero_copy.py` | the VPU's reach, imports in the log, a misaligned picture through the backend's own frame, a decoder's large input packets imported and bit exact, no import trouble |
| `test_low_memory.py` | short jobs of every kind with the RAM the VPU reaches nearly gone, held on purpose: inside their block or refused, and nothing left on the VPU, in about three minutes |
| `test_resets.py` | the missing codec drawer, a reboot resumes, a reboot during a take, a video and sound, ABANDON then repair, the external reset by hand |
| `test_profile.py` | the timing round (8.6) |

Every module ends with `test_nothing_left`: no session, no stray message.

### 8.4 The run directory

`tests/runs/<timestamp>/` (or `--run-dir`) holds:

| File | What |
|---|---|
| `report.md` | every test's outcome, time and notes, the files replaced, the baseline warnings and the values measured |
| `run.json` | the options and the build's flavour |
| `serial.log` | the whole capture |
| `measured.json` | the numbers measured |
| one directory per test | the rigs' outputs, the pictures fetched, the screenshots and its slice of the debug log |

pytest's own `-ra` summary names every failure and skip.

The report's header says what the firmware saw of the Pi's power after each
module (`get_throttled`'s lasting bits): under-voltage there points at the
supply, not the code. A Pi restart clears them, so a dip that restarts the Pi
shows only as the restart (`VCInfo`: the VideoCore had restarted).

Emu68 writes its own lines to the same UART. When it and a driver write at once
the bytes interleave and both lines are lost, so a few percent of the lines in
a busy run are garbled. The report says how many, and a failed log check says
when its slice was hit.

### 8.5 Baselines

`tests/baselines.json` holds measured numbers per machine (frame rates, PSNRs,
times), nothing else. What a number means is said where it is checked, in the
test, with an absolute tolerance or a percentage of the baseline:

```python
baselines.check("fullscreen.play.fps", fps, higher=True, tolerance="10%")
```

A value with no baseline for this machine is recorded with a warning.
`--update-baselines` writes this machine's measured values into the file at the
end; review the diff before committing it.

Pure correctness stays as plain asserts: bit-exact digests, zero mismatches,
error codes, the descriptor routing. Every timing the tests judge is measured
on the Amiga (`get_time()` microseconds in the rigs' output); host-clock times
are recorded, never asserted.

### 8.6 Timing

Where a frame's time goes, for when playback is slower than it should be.
Build the component at the profile tier: only the timing lines go to the debug
output, so the rates stay near a release build's.

```sh
EMU68_CONFIGURE_ARGS="-DEMU68_DEBUG_BACKEND=pistorm -DEMU68_TIER=off -DEMU68_TRACE= -DEMU68_DEBUG= -DEMU68_PROFILE=emu68-vchiq-library" \
    ./scripts/docker-build.sh --target emu68-vchiq-library
tests/.venv/bin/pytest tests --profile -m profile --update-baselines
```

Each instance reports every 2 s while it works, and once more when a session
(`[videocore]`) or a unit (`[vchiq]`) closes, as `[prefix] slot: n=.. sum=..us
avg=..us max=..us`. `tests/harness/perf.py` reads them; emu68-common's
`scripts/perf-report.py` reduces a capture by hand.

| Slot | What it times |
|---|---|
| `[vchiq] rx_bulk` | a receive, from queued at the VPU to done: the DMA and the device's turnaround |
| `[vchiq] rx_prep`, `rx_inval` | cache maintenance before and after a receive; none for buffers in the RTG card's VRAM |
| `[vchiq] tx_clean` | cache maintenance before a transmit |
| `[videocore] rx_parked` | a payload's wait in the client before its receive request goes out |
| `[videocore] rx_payload` | from a buffer's header arriving to its payload landed |
| `[videocore] out_vpu` | an output frame's whole time at the VPU, sent to back |
| `[videocore] out_client` | an output frame in the client's hands (shown, cooling) |
| `[videocore] in_vpu` | an input packet's time at the VPU |
| `[videocore] poll` | one `VC_Poll()` |
| `[videocore] img_call` | one image call (`VC_DecodeImage()` and the others), whole; the call reports its own lines at its end |
| `[videocore] img_open`, `img_wait`, `img_close` | one of its sessions opened, waited for, closed |
| `[videocore] mmal_open`, `mmal_close` | the `mmal` service opened (first component), closed (`VCB_Idle()`) |
| `[videocore] component`, `destroy` | a component created and its ports read; stopped and destroyed |
| `[videocore] import`, `release` | a port's buffers imported into the VPU; the wait for their release |

`test_profile.py` plays 300 frames six ways: the decoder alone, full screen
paced and unpaced, a 1600x900 and a default window, full screen at 16 bits.
Each run's rate and every slot's average go to the baselines
(`profile.<case>.*`).

### 8.7 Adding a test

A test is a function in the module of its area. It asks for what it needs:

| Fixture | Gives |
|---|---|
| `device` | the Amiga: `run()` for a rig that speaks the grammar, `run_text()` for VCInfo and the examples, `start()` for a background job, `download()`, `screenshot()`, `window()` |
| `material` | `.ram(name)` uploads a file once per boot and returns its `RAM:hw` path; `.path(name)` makes it on the host |
| `baselines` | `check()` (8.5) |
| `driver_log`, `debuglog` | a slice of the log: `since(mark)`, `.expect()`, `.forbid()`, `.find()` |
| `artifacts` | the test's directory |
| `record_property` | a note for the report |
| `operator` | `ask()`, for the listen and hands tiers |

The rules:

* inputs from fixtures, never from another test's side effects;
* temporary files in `RAM:hw` only, deleted by the test; `test_nothing_left`
  catches what is left behind;
* facts read from the rigs' `key=value` lines, never from prose;
* measured numbers through `baselines.check()`, verdicts as plain asserts in
  plain words.

The host-only tests in `tests/unit/` cover the parser, the picture maths, the
baselines and the material recipes.

### 8.8 Test material

Nothing binary is checked in. Everything comes from one sample, convertico's
`sample-mp4-1080p-h264-10s.mp4` (convertico.com/samples/mp4: free for testing,
no attribution required; 1920x1080 High, 30 fps, 300 frames with B-frames,
x264). `tests/harness/material.py` downloads it once into `tests/.cache/source/`
and checks its SHA-256 on every use: a changed file is an error, since the clip
and every baseline come from it. Offline, put the file there by hand.

The test clip, `clip42.h264`, is the sample's own stream with its SPS's level
5.0 rewritten to 4.2 (the decoder row's; the pictures stay the same):

```sh
ffmpeg -i sample.mp4 -an -c:v copy -bsf:v h264_mp4toannexb,h264_metadata=level=4.2 -f h264 clip42.h264
ffmpeg -v error -i clip42.h264 -fps_mode passthrough -f framemd5 clip42.framemd5
```

H.264 decoding is bit-exact by the standard, so ffmpeg's per-frame digests are
the reference (`-vsync passthrough` before ffmpeg 5.1). Compute them from the
elementary stream, never from the MP4 (edit lists can drop leading frames).
JPEG decoding is not bit-exact between decoders, so pictures are compared by
PSNR.

The rest is made on the host from `clip42` by `tests/harness/material.py`
(ffmpeg, x264, Pillow) into `tests/.cache/material-v<version>/` the first time
a test needs it, and kept: the converter and encoder inputs, the JPEGs, the
shape-change and pixel-aspect streams, the H.264 limit probes, the still
pictures of every format with Pillow's decode of each as the reference
(`tests/harness/stills.py`), and the colour-bar pattern the screenshot tests
judge. The VPU's H.264 output keeps the limited range, 16 to 235, so a bar
passes at either rendering. Two inputs the Amiga makes itself, once per boot:
one frame and thirty frames of clip42 decoded to raw I420.

The ffmpeg command lines are the test material: a changed recipe gets a new
`MATERIAL_VERSION`, so the old cache is left alone.
`tests/.venv/bin/python tests/harness/material.py` makes everything ahead of a
run (about ten minutes, mostly x264); `--only l-` makes what matches.
