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
