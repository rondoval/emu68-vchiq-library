# Release notes - emu68-vchiq-library 0.5

- **More pixel formats through the VideoCore converter.** Besides I420, the RGB
  formats, NV12 and YUYV, it now reads and writes NV21, planar 4:2:2 (I422), the
  packed 4:2:2 orders UYVY, YVYU and VYUY, and 32-bit RGB without alpha (RGBX32,
  BGRX32), and writes YV12: a camera's or a program's own pictures convert
  without a copy through the 68k. Each was checked against ffmpeg's conversion.
- **For programmers: more format numbers.** `libraries/videocodec.h` also names
  ARGB32, ABGR32, the big-endian and 15-bit RGB formats of Picasso96, CLUT8 and
  the 10-bit column format, for codec drivers to come; the VideoCore refuses them.
- **For programmers: codecs by FourCC.** `VC_CodecOfTag()` tells which codec a
  container's FourCC names (`XVID`, `hvc1`, `DIV3`, ...), so a player needs no
  table of its own, and `VCA_CodecTag` passes it with a session, so that a
  codec driver serving only some forms of a codec is chosen only for those.
  `VCInfo CODECS` shows such a row's FourCC.
- **For programmers: more codec numbers.** `libraries/videocodec.h` names
  MPEG-1, MPEG-2, MPEG-4 Part 2, MS-MPEG4, H.263, VP8, VP9, AV1, VC-1,
  Theora, Cinepak, Microsoft Video 1 and QuickTime Animation, each with how
  its packets are cut and what its extradata holds, for codec drivers to come;
  none decodes them yet.
- **H.265 video shows on screen.** A decoder whose own pictures a program
  cannot draw, the H.265 backend's column pictures, gets the VideoCore
  converter placed behind it by `videocodec.library`: the program opens one
  session, asks for the RGB format and size it wants, and gets them, as it
  does for H.264. `VCTest DECODE` takes `.h265` files and the `H265` encoding
  name, and the example player `VCPlay` plays `.h265` and `.hevc` files.
- **For programmers: what a decoder can be asked for.** `VC_QueryCodec()`
  lists, in a decoder row's new `ChainOutputFormats`, the formats its
  sessions write through that converter, so a program can choose its screen
  format before it opens a session. `VCInfo CODECS` shows them as "chained".
- **For programmers:** backend ABI 1.1 adds `VCB_OutputFrame()`, a session's
  own output frames by index; the frontend imports them into the converter it
  chains. Output frames can be imported into a chained session once
  `VCEV_StreamInfo` came. The column format `VCF_NV12_COL128` is settled.
- **A picture on screen survives a size change.** When an H.265 stream
  changes its picture size, the picture a player still shows stays valid
  until the player gives it back: the converter of the old size stays open
  for it, and closes then.
- **For programmers: a session can change hands.** `VC_SetAttrsTags()` with
  `VCA_SigTask` or `VCA_SigMask` makes the VideoCore signal another task or
  signal from then on, as the H.265 backend already did; a player can hand a
  session to another task.

---

# Release notes - emu68-vchiq-library 0.4

- **Camera pictures on the VideoCore.** The converter now takes a USB
  camera's uncompressed YUY2 and NV12 pictures, so Neptune's USBVideo can show
  a live picture of those modes too. `VCInfo CODECS` lists what the converter
  reads.
- **For programmers:** `VCCodecInfo.InputFormats` names the pixel formats an
  encoder or the converter reads, and others are refused at the open.
  `Formats` is now `OutputFormats`, what a decoder or converter writes.
- **VCGrab** records as before. Its Matroska writer now lives in a project of
  its own, [mkv-writer](https://github.com/rondoval/mkv-writer), which
  Neptune's USBVideo uses too.

---

# Release notes - emu68-vchiq-library 0.3

First public release. Video, JPEG pictures and sound run on the Raspberry
Pi's VideoCore chip, for AmigaOS on a PiStorm with a Pi 4 or CM4, and the 68k
stays almost idle.

**Experimental.** The stack's Install script offers its three parts
(`videocodec`, `videocore.audio`, `vcjpeg.datatype`) unticked. The
[README](README.md) has the setup.

---

## What's in it

- **Hardware video.** The VideoCore decodes H.264 and MJPEG and scales each
  picture to the screen. A version of MintVID plays 1080p full screen at 30 fps
  with sound. MintVID itself does not have this yet, and may never (README,
  Playing a video).
- **Faster JPEG pictures in every program.** `vcjpeg.datatype` decodes
  baseline JPEGs of about 2 megapixels and more for MultiView, Workbench and
  anything else that loads pictures through datatypes: twice as fast as
  software at 5 megapixels, three times at 24. Other JPEGs stay with your
  JPEG datatype.
- **Sound through the Pi.** `videocore.audio`, an AHI driver, plays every AHI
  program to HDMI0, HDMI1 or the headphone jack, in stereo or 7.1.
- **Screen recording.** `VCGrab` records an RTG screen to an MKV file, and
  grabs stills, while the VideoCore does the work: 1080p at 30 fps takes about
  5% of the 68k. It is a commodity in `SYS:Tools/Commodities`, with an icon, a
  settings window and hot keys, and works from the shell too.
- **`VCInfo`** checks the setup part by part and says what to fix.
- **`videocodec.library`** gives programs all of this and more: decoding
  JPEG, PNG, GIF, BMP, TGA and PPM pictures, encoding H.264 and JPEG, scaling
  and converting pictures.

---

## Known limitations

- **Needs an Emu68 newer than 1.1.0-beta.1**, with `dtoverlay=cma` in
  `config.txt`: the tagged releases do not have that overlay yet.
- **H.264 is 8-bit, up to 1920x1080** or 1080x1920 upright. Larger or 10-bit
  video plays in software, slowly.
- **Full screen at 1080p needs 32 MiB of RTG memory**
  (`dtoverlay=videocore,mem=32`); with less it plays at about 17 fps.
- **The default `gpu_mem` plays one ordinary 1080p video or records a 1080p
  screen.** Two videos at once, a video during a recording, JPEGs over about 24
  megapixels through `vcjpeg.datatype` and videos with more than about 10
  reference frames need `gpu_mem=128`.
- **If the VideoCore stops answering, press Ctrl-Amiga-Amiga.** That restarts
  the Pi and the VideoCore with it; `C:Reboot` restarts only the 68k. A crash
  during a video or a recording can cause this. If the connection still does
  not come back, power off and on.
- **Not tried yet:** the Pi 3, and the headphone jack and the 7.1 channel
  order by ear.
- `vcjpeg.datatype` needs AmigaOS 3.2.
- `VCGrab` has no sound and no mouse pointer, and ends on screens that flip
  between bitmaps, as many games do.

---

## For developers

The archive's `Developer/` drawer has the headers, a minimal video player and
picture viewer with their sources, and the test tools. The API, the codec
driver interface and the build are in
[developer-guide.md](developer-guide.md), in the source repository.
