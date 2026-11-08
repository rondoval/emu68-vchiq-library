# emu68-vchiq-library

> **Releases:** this component ships as part of the
> [emu68-driver-stack](https://github.com/rondoval/emu68-driver-stack): the downloadable
> `.lha` and bundled documentation are published there. This repository is source-only
> and versioned via git tags.

Hardware video, JPEG pictures and sound for AmigaOS on a PiStorm with a
Raspberry Pi 4 or CM4. The Pi's VideoCore chip does the work and the 68k stays
almost idle: a 1080p video goes straight into your RTG screen without the 68k
touching a pixel.

> **Experimental.** This is the first release. It has run on real hardware in
> a scripted test cycle, but expect rough edges: see
> [Known limitations](#known-limitations).

---

## What you get

- **Hardware video.** The VideoCore decodes H.264 and MJPEG and scales each
  picture to the screen. [A version of MintVID](#playing-a-video) plays 1080p
  full screen at 30 fps with sound.
- **Faster JPEG pictures in every program.** `vcjpeg.datatype` decodes large
  JPEGs on the VideoCore for MultiView, Workbench and anything else that loads
  pictures through datatypes.
- **Sound through the Pi.** `videocore.audio`, an AHI driver, plays every AHI
  program to HDMI or the headphone jack.
- **Screen recording.** `VCGrab` records an RTG screen to an MKV file and grabs
  stills, the VideoCore doing the work and the Amiga carrying on undisturbed.
- **`VCInfo`** checks the setup and says what to fix.
- **`videocodec.library`** gives programs all of this and more: decoding JPEG,
  PNG, GIF, BMP, TGA and PPM pictures, encoding H.264 and JPEG, scaling and
  converting pictures.

---

## Requirements

- A **Raspberry Pi 4 or CM4** on a PiStorm. (Pi 3 support is written but
  untested.)
- **Emu68 newer than 1.1.0-beta.1**: the tagged releases do not have the `cma`
  overlay yet.
- **AmigaOS 3.x**; `vcjpeg.datatype` needs AmigaOS 3.2.
- **Picasso96 and an RTG screen** for video.
- **AHI 4 or 6** for sound.

And these lines in `config.txt` on the Pi's boot partition:

| Line | What it is for |
|---|---|
| `dtoverlay=cma` | Always needed: the memory shared with the VideoCore. The default 4 MiB is plenty. |
| `dtoverlay=videocore,mem=32` | Full-screen video at 24 or 32 bits. With Emu68's default 16 MiB, 1080p plays at about 17 fps. |
| `gpu_mem=128` | Optional. The default of 76 plays one 1080p video with up to about 10 reference frames (most have 4 or fewer), or records a 1080p screen, and decodes JPEGs up to about 24 megapixels. 128 adds a second video, a video during a recording, up to 16 reference frames, and JPEGs up to about 60 megapixels. Two videos of 16 reference frames need 164. Sound does not need it. |

Emu68 reads `config.txt` only when the Pi starts, so press Ctrl-Amiga-Amiga or
power-cycle after changing it. Memory given to `gpu_mem` is taken from the
Amiga: 128 costs it 52 MiB of Fast RAM. `VCInfo` shows what is free: `gpu_mem` less 25 MiB when nothing plays. A
1080p video uses about 30 MiB, 68 with 16 reference frames, and recording a 1080p screen about 33.

---

## Installation

Tick what you want in the stack's Install script. `vcjpeg.datatype` brings
`videocodec` along, and `videocore.audio` brings `vchiq.device` and `VCInfo`,
which are all of it that sound needs. Take the `-rangeops` archive: an Emu68 new
enough for this has the dcache extensions.

| Tick | File | Installed to |
|---|---|---|
| `videocodec` | `vchiq.device` | `DEVS:` |
| | `videocodec.library` | `LIBS:` |
| | `videocore.codec` | `LIBS:videocodec/` |
| | `VCInfo` | `C:` |
| | `VCGrab` and its icon | `SYS:Tools/Commodities/` |
| `videocore.audio` | `videocore.audio` | `DEVS:AHI/` |
| | `VIDEOCORE` | `DEVS:AudioModes/` |
| `vcjpeg.datatype` | `vcjpeg.datatype` | `SYS:Classes/DataTypes/` |
| | `VCJPEG` | `SYS:Storage/DataTypes/` |

To install by hand, copy the files as above. For `vcjpeg.datatype`, also add
this line to `S:User-Startup`:

```
AddDataTypes SYS:Storage/DataTypes/VCJPEG QUIET
```

Then reboot, and check the setup:

```
VCInfo
```

It checks each part in turn, says what to fix, and ends with `PASS`.
`VCInfo CODECS` lists what the hardware can do.

---

## Playing a video

[MintVID](https://github.com/boingball/MintVID) plays video files (MP4, MKV,
AVI, MPEG-TS and more) with their sound. A version of it that hands H.264 and
MJPEG to the VideoCore whenever `videocodec.library` is installed, and decodes
the rest in software, is in the `videocore-decode` branch of
[rondoval/MintVID](https://github.com/rondoval/MintVID). It has been offered
to MintVID's author, so it may reach a MintVID release, later or not at all.

The VideoCore takes 8-bit H.264 up to 1920x1080, or 1080x1920 upright, and
keeps up with 1080p at 60 fps, and MJPEG up to 4096 wide and 1920 tall. Larger
and 10-bit videos play in software, slowly. H.265 needs the stack's
`rpivid.codec`, on the HEVC block of a Pi 4 or CM4: its README has what it
plays. Re-encode the rest on a PC (`scale=-2:1920` for an upright one):

```sh
ffmpeg -i video.mp4 -vf scale=1920:-2 -c:v libx264 -pix_fmt yuv420p -crf 20 -c:a copy video-1080p.mp4
```

---

## Recording the screen

`VCGrab` is a commodity in `SYS:Tools/Commodities`. Double-click its icon, or
put a copy in `SYS:WBStartup` to have it ready after every boot. Ctrl-Alt-R then
starts recording the RTG screen in front to an MKV file, Ctrl-Alt-S stops, and
Ctrl-Alt-G grabs a still as a JPEG, which costs a recording a picture or two.

Ctrl-Alt-V, or Show in Exchange, opens its settings window:

| Setting | What it does |
|---|---|
| Drawer, Name | Where takes go. Each gets the next free number: `Work:take-0001.mkv`, `Work:take-0002.jpg`. `RAM:grab` until you choose |
| Size | As the screen, or scaled to one of the sizes offered |
| Pictures a second | 1 to 60, and 1080p stops at 30 |
| Bit rate | In kbit/s, up to 25000; 0 lets the encoder choose, about 10000 |
| Time limit | Ends a recording after so many seconds; 0 for no limit |
| Start, Stop and Still key | The hot keys, written like `ctrl alt r`. Leave one empty to have no key for it |

Record, Stop and Still do what the keys do, with the settings the window shows.
Record starts on the screen in front, usually Workbench, with this window in the
picture. Save keeps the settings for good, Use until the next reboot, and Cancel
forgets them. Settings changed during a recording hold from the next take. The
line above the buttons tells how the last take went, or why it could not start.

The icon's tooltypes are the usual ones: `CX_POPKEY` for the key that opens the
window, `CX_POPUP=NO` to start without showing it, as a copy in `SYS:WBStartup`
should, and `CX_PRIORITY`.

From the shell, with its full name, it records one file until `SECONDS` runs out
or you press Ctrl-C, or grabs one still:

```
SYS:Tools/Commodities/VCGrab Work:demo.mkv SECONDS 60
SYS:Tools/Commodities/VCGrab Work:shot.jpg STILL
```

`SIZE 1280x720`, `FPS 25` and `BITRATE 8000` are the settings above, and
`SCREEN name` names a public screen, which must be in front too. These two ignore
what the window saved, so a script does what it says. `CX` starts the commodity
instead, without a window unless you add `CX_POPUP YES`. Its arguments then hold
over the saved settings for that run, and `TO` is optional:

```
Run >NIL: SYS:Tools/Commodities/VCGrab Work:take CX
```

- The recording pauses while another screen is in front, and the file has a
  gap there.
- A crash or a full disk leaves a file that plays up to that point.
- A file stops at about 4 GiB: 55 minutes at the default bit rate, 22 at the
  highest, and often far longer, as a quiet screen takes much less.
- Larger screens are scaled down to 1920x1080.

Not recorded: sound, the mouse pointer (the graphics card draws it, it is not
in the screen's picture), native Amiga screens, and screens whose rows the
VideoCore cannot read (those not of whole 32 pixels: 1366 wide, say). A screen
that flips between bitmaps, as many games do, ends the recording. Where the
Amiga is drawing just as a picture is taken, that picture can show a tear.

---

## Pictures

`vcjpeg.datatype` takes baseline JPEGs of about 2 megapixels and more, where
the VideoCore beats the 68k. The time to load a picture on a Pi 4 at
`gpu_mem=192`, against AmiKit's `jfif.datatype`:

| Picture | `jfif.datatype` | `vcjpeg.datatype` |
|---|---|---|
| 1920x1080 | 125 ms | 114 ms |
| 2560x1920 (5 MP) | 335 ms | 171 ms |
| 4000x3000 (12 MP) | 950 ms | 387 ms |
| 6000x4000 (24 MP) | 1930 ms | 601 ms |

Everything else stays with the JPEG datatype you have: smaller, progressive
and CMYK pictures, and any the VideoCore has no memory for: at the default
`gpu_mem`, anything over about 24 megapixels, or over about 3 while a video
plays.

Its descriptor stays in `SYS:Storage/DataTypes/`, not `DEVS:DataTypes/`: it
has to be added after the system's JPEG descriptors to be tried first, and
the line in `S:User-Startup` does that. Delete the line and reboot to go back.

---

## Sound

In the AHI preferences, pick a VideoCore mode for a unit and choose its
output.

| Mode | |
|---|---|
| `VideoCore:16 bit stereo++` | The everyday mode. |
| `VideoCore:HiFi 16 bit stereo++` | Better with many channels, a little more work for the 68k. |
| `VideoCore:HiFi 16 bit 7.1 multichannel` | Eight channels for an AV receiver on HDMI. Needs AHI 6 and a program that plays 7.1. A stereo display plays only the front pair. |

| Output | Plays to |
|---|---|
| `HDMI0` (the default) | the HDMI port next to the power socket on a Pi 4 |
| `HDMI1` | the second HDMI port |
| `Headphone jack` | the headphone jack (on a CM4 it depends on the carrier board) |

All modes take 8000 to 48000 Hz, and several programs can play at once. The
unit's output volume in the AHI preferences sets the VideoCore's.

---

## Known limitations

- Sound latency is about 40 ms, and recording is not supported.
- A video that changes its picture size partway is scaled into frames sized
  for its first picture, so a change between 4:3 and 16:9 stays stretched.
- Two 1080p videos play smoothly in windows. Full screen beside a window keeps
  going, but most of its frames come late.
- If the VideoCore stops answering (a video freezes, a picture fails with
  error `-7`), press Ctrl-Amiga-Amiga. That restarts the Pi and the VideoCore
  with it; `C:Reboot` restarts only the 68k. A crash during a video or a
  recording can cause this.
- Not tried yet: the headphone jack and the 7.1 channel order by ear.

---

## Troubleshooting

| Symptom | Cause and cure |
|---|---|
| `VCInfo` says `cma.resource missing` | Add `dtoverlay=cma` to `config.txt`. |
| The VideoCore connection does not come back after a reboot, or after `Avail FLUSH` during a video | Press Ctrl-Amiga-Amiga. If it still does not come back, power off and on. The firmware starts its side of the connection only once per Pi start, and it cannot always be taken over. |
| Error `-8` as a video starts | The video is 10-bit, 4:2:2 or too large. Re-encode it (see [Playing a video](#playing-a-video)). |
| Error `-2`, or MintVID plays a video slowly, in software | The VideoCore is out of memory: a second video, one with many reference frames, or a video during a recording. Close the other video, or set `gpu_mem=128` in `config.txt`; `VCInfo` shows what is free. |
| Colours banded on a 16-bit screen | The hardware does not dither. Play on a 24- or 32-bit screen. |
| The VideoCore modes are missing from the AHI preferences | Reboot, or run `AddAudioModes REFRESH`. |

---

## For developers

The archive's `Developer/` drawer holds the headers, a minimal video player
and picture viewer with their sources (`Examples/videocodec`), and the test
tools (`Tools`). None of it is installed.

In the source repository, [developer-guide.md](developer-guide.md) covers the
API, what each picture format accepts, writing a codec driver, the tools,
building and the hardware tests, and
[docs/architecture.md](docs/architecture.md) explains how it works and why.

## License

BSD-3-Clause (see `LICENSE`). The transport follows the Linux VCHIQ core
(GPL-2.0 OR BSD-3-Clause, Broadcom) and the codec driver Broadcom's
BSD-3-Clause `userland`.
