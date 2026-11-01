# emu68-vchiq-library: firmware facts

What the VideoCore firmware and the platform under it actually do: measured on a
Pi 4 and a CM4 unless a source is named. The firmware documents little of this
and refuses almost nothing, so these are the facts behind the rules in
[architecture.md](architecture.md) section 9. `AGENTS.md` lists what must never
be done. Add a fact here when a measurement finds one.

---

## 1. Video decoders

* **H.264 limits** (`tests/test_levels.py`). The level the SPS declares is not
  checked (3 to 6.2 decode alike), and 16 reference frames at 1080p decode bit
  exact. The real limits are the picture and the rate: at most 2032 pixels wide
  (127 macroblocks) and 8192 macroblocks (level 4's frame size; 1080x1920 fits),
  8-bit 4:2:0 only, Annex-B only.
* **Past them it does not refuse.** A wider picture's predicted pictures come out
  wrong, a larger one's rows past the limit, High 10 approximately, and High
  4:2:2 gives no picture.
* **Speed.** 1080p at 84 fps at low rates, 67 fps at level 4.2's ceiling (1080p60,
  64 Mbit/s), 31 fps at level 5.0's, so `VCCodecInfo.MaxLevel` is 4.2.
* **The starting size.** A decoder started at the stream's own size raises no
  FORMAT_CHANGED, and its first picture lands in the token. One started at 16x16
  always raises it (H.264 and JPEG alike).
* **The pixel aspect.** The H.264 decoder's format change says 1/1 whatever the
  stream's SPS says (predefined and extended aspects alike), and MMAL has no
  other way to ask.
* **A stream that starts between key frames** (`VCTest DECODE FRAMED`, C930e and
  x264 streams with frames cut out). Nothing comes out and no error
  is raised until the first key frame it has an SPS for; every picture from there
  on is the intact stream's, whether or not an SPS came ahead of the predicted
  frames. With no SPS at all it never starts, says nothing, and the end mark gets
  no answer.
* **A hole in a stream** (the same runs). Predicted frames or a key frame lost, it
  goes on without an error, one picture per frame, damaged until the next key
  frame, which is exact again.
* **Several slices a frame are not quite bit exact.** Constrained Baseline with 4
  slices a frame, the C930e's form: about 130 bytes of a 320x180 frame differ
  from ffmpeg, each by 1, in 9 frames of 170 from the camera and all of them from
  x264. Cause not looked at.
* **JPEG through the video decoder** (MJPEG). Baseline only, "no support for
  progressive jpeg encode or decode"; the engine is unchanged from the Pi 0 to
  the Pi 4. Its output is YUV, so RGB needs the ISP. It joins no pieces. Unless
  the input is marked framed and each packet a whole picture, it finds a
  picture's end only at the next one, so a lone picture never comes out. Before
  each packet was marked, it decoded a picture only with the end mark on the
  same packet, and an empty end mark decoded nothing, framed or not. That has
  not been repeated since the marks went in.
* **MJPEG's limits** (`tests/test_levels.py`). It takes 4096 pixels across and 1920
  down, whatever the area. Past the width the picture comes out a plain grey. One
  row past the height it reports 1920 and the last row is gone, and by 1928 rows no
  picture comes and no error either. Within them it softens edges on 8x8 block lines
  and overshoots them a little, which ffmpeg does not: 32 to 36 dB against ffmpeg on a
  test pattern.

---

## 2. Tunnels and the ISP

* **The tunnel's start order.** The decoder gives its first picture as soon as
  its end of the tunnel runs. With the ISP's input enabled first (which enables
  that end) and its output after, 9 of 18 starts lost that picture and gave the
  next one 500 ms late, the decoder taking input all the while: 720p and 1080p,
  with or without timestamps, framed or not, the host's calls the same in both
  outcomes. A stream then came out one picture short. With the output running
  and its frames out first, all 18 started at once with every picture.
* **The end mark behind the ISP.** Through an I420 tunnel, a picture that reaches
  the ISP carrying the end-of-stream flag comes out as a bare end mark, the
  picture dropped. The H.264 decoder sets the flag on its last flushed picture
  whatever is sent (an empty end mark, an end-of-sequence or end-of-stream NAL
  alike). The opaque tunnel keeps that picture.
* **A change of shape in a running tunnel.** The VPU's MMAL core
  commits it on the decoder's output and passes the event down the tunnel; the
  host gets no FORMAT_CHANGED. With I420 between them the ISP ignores the event
  and keeps the first shape: a smaller picture comes back as an empty buffer, a
  bigger one as stripes. With opaque pictures it follows both ways, every frame
  right.
* **Flushing an opaque tunnel.** A flush of its decoder end never answers, and
  the VPU's MMAL service is gone until the Pi restarts. A flush of its ISP end
  alone clears the pictures between them.
* **The ISP tunnelled into the encoder.** `ril.isp`'s I420 output connects to
  `ril.video_encode`'s input and runs at 1080p30, all five input formats. Each
  picture's time goes through, gaps included. The encoder's `color_space` still
  reaches the SPS. A picture with the end flag comes out whole here, and an
  empty buffer with the flag ends the stream with none added.
* **The tunnel behind.** With the encoder behind, the ISP keeps its input until
  the tunnel has room, so no picture is lost: 60 of 60 at 25 Mbit/s of noise,
  frames coming back late instead. The two components and the tunnel hold about
  eight pictures ahead of the packets.
* **The tunnel stopped.** With the host keeping the encoder's packets, closing
  takes 18 ms at worst: the disable answers, and the next session runs. A bit
  rate past level 4.0 fails at the first enable, which unwinds the same way.
* **ISP output formats.** RGB565, RGB24, BGR24, BGRA32, RGBA32, YUV420; never
  ARGB, so a 32-bit Picasso96 mode must be B8G8R8A8 or R8G8B8A8. Output alpha is
  always 255. The ISP does not dither: RGB565 bands on gradients.
* **ISP rows.** Whole groups of 32 pixels, heights even; I420 rows, in and out,
  whole 64 (its half-width chroma rows whole 32): it reads a 4000-pixel row as
  4032. The converter's own I420 input has such rows. The image decoder writes
  its own rows whatever width its output port is given (a JPEG's I420 in whole
  32), so a JPEG whose rows are not whole 64 reaches the converter as RGBA.
* **RGB to YUV in the ISP.** Always BT.601 full range: `color_space` on the
  input port, the output port or both changes nothing (the bars of
  `tests/test_vcgrab.py`).
* **The ISP reads the screen.** VRAM imported as an input works, live, and rows
  of 1080 packed lines are taken without padding, all five formats.
* **The encoder's colour space.** `ril.video_encode` writes the `color_space` of
  its input port into the SPS's VUI: `pc, smpte170m` for JPEG_JFIF, `tv, bt709`
  for BT709, none when unset. RGB straight into it works, as limited BT.601,
  with red and blue swapped as at the ISP.
* **The encoder's bit rate.** Above level 4.0's 25 Mbit/s the input port's enable
  fails with EINVAL. Noise comes to 72 Mbit/s over its first 2 seconds, and to
  the 25 asked for over 15. Packets have a minimum of 64 KB at 1080p. A key frame of
  noise is 0.8 to 1.3 MB and comes in pieces: all of one Pts and key flag, the
  first with the configuration flag, only the last with the frame end.
* **The ISP's 32-bit input.** It reads an RGBA input as BGRA and the reverse; its
  output is right.

---

## 3. The image decoder

`ril.image_decode` (`VCInfo COMPONENTS` and probe sessions) takes
JPEG, PNG, GIF, BMP, TGA and PPM in any pieces, at least 2 buffers of 80 KiB.

* **Alone** its output is only its own format (I420 for a JPEG, RGB565 for a GIF,
  RGBA otherwise) whatever its port reports after a commit: asked for another it
  writes its own (PNG) or raises an error (JPEG), after which the next session's
  MMAL stopped answering.
* **Behind `video_convert`** (`aggregator.pipeline:ril.image_decode:video_convert`,
  one component on the VPU, no BGRA) it writes I420, RGB565 or RGBA from any of
  them. The pipeline tells the shape once per stage (the same size again must be
  kept, a remake stalls it) and will not run with less than its default picture's
  buffer although it reports none. Rows: whole 16 pixels (a JPEG's: whole 32, its
  I420 stage), whole 16 rows.
* **The end mark.** The picture comes back with the end mark; without one after
  its last piece the decoder never gives the picture.
* **It refuses with an error** progressive and CMYK JPEG; grey, 16-bit and
  interlaced PNG; and a JPEG whose frame header comes past about 80 KiB (comments
  before it: 79 KB decoded, 90 KB refused whatever the packet size; 120 KB of
  comments after the frame header decoded with the header in one packet). Phones
  write 89 KB and more. Leaving out the header's longest run of metadata gets
  most of those JPEGs through; APP0 and APP14 stay, since they say how the
  samples are coded.
  Refused at the first packet, before any picture, the firmware recovers.
* **A run-length TGA takes the whole firmware down**, gencmd too, until a Pi
  restart.
* **A still picture must not fail mid-picture.** Closing the pipeline while it
  decodes (a refusal at FORMAT_CHANGED of an 8000x6000 picture, a frame too small
  for the decoder's rows, a host with no memory left for the frame) left the
  firmware's MMAL dead, its memory held, until a Pi restart. A picture cut short
  would do the same: the decoder waits for the rest and is closed holding part of
  it.
* **It gets wrong:** a PNG whose height is no multiple of 16 has its last
  `h mod 16` rows written `16 - h mod 16` rows too high (those rows lost); the
  transparent colour of a GIF or an RGB PNG comes out opaque (a palette PNG's
  alpha levels come out right); a GIF stays at RGB565 precision.
* **An animated GIF** raises an error event after its first picture came out,
  every time. The picture is right and the next session runs.
* **A kept pipeline.** After its ports and the component are disabled
  it decodes the next picture right, but its output keeps the last picture's size
  whatever format is committed, so a picture of that size raises no FORMAT_CHANGED
  and fills the first output buffer. Flushing its ports first changes nothing.
  `ril.isp` is reused as it is.
* **Big pictures.** An 8000x6000 JPEG to I420 (72 MB) decodes in 0.6 s, a
  6016x4000 one to RGBA (96 MB) in 0.5 s. One bulk transfer carries at most 126
  MB (vchiq.device's page-list slots: 126 runs of 256 pages on the BCM2711).
  Imports have no such limit: 8000x6000 to RGBA (192 MB) in 1 s, 8192x6144 to
  RGB24 (151 MB) with every row right.

---

## 4. Memory: `gpu_mem`

`gpu_mem` (config.txt) is the VPU's own memory: the firmware's shadow of the output
pools that are not imported, and the decoder's pictures, which the tunnel passes.
It is unrelated to the RTG VRAM (`vc4-mem`, 0x32000000-0x34000000 on the PI4, sized
by the `videocore` overlay's `mem=`). The firmware's own heap (16 MB) does not
move. The relocatable heap the codecs use is `gpu_mem` less 20 MB, 5 MB of it in
use idle:

| `gpu_mem` | Heap | Free idle |
|---|---|---|
| 192 | 172 MB | 167 MB |
| 88 | 68 MB | 63 MB |
| 76 (the Pi 4's default) | 56 MB | 51 MB |

* **A decoder** takes 1.75 bytes per pixel for each of its reference frames plus
  4 MB (measured at 1080p, 720p and 480p, 1 to 16 frames, within 3 MB).
* **A 1080p decoder with its ISP** takes about 3.4 MB per reference frame plus 13
  (4 frames 27 MB, 8 take 40, 12 take 54, 16 take 68), the same at outputs from
  960x540 to 1600x900 in a window and on the screen, all of it back when the
  session closes. Two ordinary 1080p decoders fit at 88 and not at 76; two with 16
  reference frames take 137 MB, and one does not fit at 88.
* **An image call** needs the decoder's picture, 1.5 bytes per pixel for a JPEG,
  plus 16 MB free, and a copy of what goes by bulk. At 76 that allows 24 MP, or
  8 MP through the converter's bulk output. Kept components count as used until
  a picture that would not fit lets them go.
* **Too little is silent.** It shows as `VCERR_DEVMEM` where the firmware says so,
  but a decoder it has no memory for takes its input and hands it straight back,
  undecoded, and no picture size ever comes (seen with a second 1080p decoder at
  76 MB). A stream the decoder does not take looks the same. videocore.codec holds
  the SPS against the heap, at the open with `VCA_Extradata`, else at the first
  packet. For the rest it counts the input until the picture size comes: after
  4 MB it reads the heap through gencmd and fails with `VCERR_DEVMEM` if less than
  24 MB is free; after 16 MB it fails with `VCERR_STREAM`.
* **Zero copy's saving.** A 6000x4000 JPEG through the converter needed 144 MB of
  copies in `gpu_mem`, more than `gpu_mem=192` holds.

---

## 5. Services

### `mmal`, closed with components running (`tests/test_resets.py`)

A service's close does not take the client's components with it. After a 68k
reset that closes the services under a running session, an encoder behind the
ISP or a decoder before it, the components stay on the VPU with their memory
(33 to 35 MiB of codec memory stay taken) and the imports they hold. The
connection resumes, and gencmd and the liveness probe still answer, but the next
open of `SMEM` gets no answer and neither does any open after it, until the Pi
restarts. Stopping the components first leaves nothing behind.

### `SMEM` (`tests/test_zero_copy.py`)

The firmware takes protocol version 2 (it acknowledges, then reports every import
released), imports at 64-byte rather than page alignment, and two imports of the
same memory at once (the decoder's frame as its output and the converter's input).
An import or the wait for its release is one round trip, 0.4 to 1 ms.
`MMAL_PARAMETER_ZERO_COPY` takes effect when set after the format commit, before
the enable, and the image decoder's pipeline takes it although Linux never uses it
there. A returned buffer says `is_zero_copy` and comes with no bulk, its payload at
offset 0. The VPU never asked for memory of ours (`VC_MEM_REQUEST`, refused if it
does).

`ril.video_decode` takes its compressed input the same way: H.264 and JPEG in 1
MiB imported packets decode bit exact, and a camera's frames ran so for a minute
at each size up to 1920x1080.

### `AUDS` (`tests/test_audio.py`)

Twenty-byte little-endian messages (`shared/include/auds_proto.h`, after userland's
`vc_vchi_audioserv_defs.h`); the firmware speaks version 3 and takes our 2.

* It answers CONTROL, CONFIG and CLOSE with a RESULT, and OPEN, START, STOP and
  WRITE with nothing.
* A WRITE with `max_packet` 0 announces one bulk transmit of the samples. The
  firmware copies them at once (the bulk is back in about 1 ms) and queues them
  itself.
* COMPLETE counts the bytes played, at the configured rate exactly (8000 to
  48000 Hz on HDMI0, HDMI1 and the jack alike, within 0.3%); bit 30 says it ran
  dry. With one 10 ms chunk in flight every COMPLETE carries it and 3 s of sound
  take 6.3 s; two in flight play without a gap.
* STOP (`draining` 0) with four chunks queued still reports three quarters of them
  played within 300 ms.
* HDMI1 with nothing attached plays as if it were there. The jack's PWM pins (GPIO
  40, 41) are already set to ALT0 on the Pi 4, so nothing needs muxing. Volume is an
  attenuation in 1/256 dB.
* CONFIG takes 1 to 8 channels on every output and they are consumed at the
  rate. Nothing sets their order (CONFIG's `channelmap` is version 3's, and
  Linux's driver never sends one), so videocore.audio sends a 7.1 frame in
  HDMI's order. On a stereo display only the first two channels are heard; the
  other six are dropped, not mixed in.

### `ril.audio_decode`

It takes the format of every encoding it lists (MP3, AAC, AC-3, DTS, Vorbis,
WMA, AMR) but refuses to start for any of them, so compressed sound is decoded
on the 68k.

---

## 6. The platform under VCHIQ

* **Doorbell.** `/soc/mailbox@7e00b840`: read BELL0 (bit 2 pending, the read
  acknowledges), write BELL2 to ring the VPU. Interrupt: GIC SPI 34. The init call
  is mailbox tag `0x00048010` with the slot area's bus address.
* **A second init tag** is answered with `0xFFFFFFFF`, and Raspberry Pi's engineers
  describe the Linux `kexec` equivalent as unsupported.
* **Emu68's memory types.** Device for MMIO, write-back for Fast RAM, write-through
  for the VideoCore windows (`vc4-mem`, `unicam-mem`, `gpu_mem`), and an unused
  non-cacheable type. On the Cortex-A72 the TRM (table 6-1) maps every write-through
  combination to the internal type Non-cacheable; the A53 does the same (section
  7.5).
* **Hardware resets.** Emu68's housekeeper core reboots the whole Pi (watchdog,
  `PM_RSTC` full reset) on any RESET it did not drive itself, so whether
  Ctrl-Amiga-Amiga restarts the VPU depends on the PiStorm firmware: the newer
  firmware watches `_RESET`, where the keyboard controller's reset arrives
  through Gayle (measured both ways).
* **Costs.** A round trip to the VPU in an image call is about 0.9 ms, and opening
  and closing the `mmal` service about 5 ms. `ScrollVPort()` takes 19 ms on
  VideoCore.card, which busy-polls the scan line.
