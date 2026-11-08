# SPDX-License-Identifier: BSD-3-Clause
"""
material.py - the test material made on the host: clip42.h264 from a
downloaded sample, pictures and streams from clip42 (ffmpeg, x264, Pillow),
the H.264 level probes, the still pictures (stills.py), and the colour-bar
pattern the screenshot tests judge.

The sample is fetched once into tests/.cache/source/ and checked against its
SHA-256.  Everything else is made once into
tests/.cache/material-v<MATERIAL_VERSION>/ and kept; bump MATERIAL_VERSION
when a recipe changes, so the old cache is left alone and a fresh one is made
(the sample stays).  The ffmpeg command lines are the test material: moved
verbatim from the former testdata/hwcycle.py (make_material,
make_level_material, describe).

    python3 tests/harness/material.py            make everything (about 10 minutes the first time)
    python3 tests/harness/material.py --only l-  make what matches
"""
from __future__ import annotations

import hashlib
import struct
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
from typing import Callable

MATERIAL_VERSION = "1"

TESTS = Path(__file__).resolve().parents[1]

# 1920x1080 High, 30 fps, 300 frames (x264), from convertico.com/samples/mp4: free for testing, no attribution required
SOURCE_NAME = "sample-mp4-1080p-h264-10s.mp4"
SOURCE_URL = f"https://convertico.com/samples/download.php?format=mp4&file={SOURCE_NAME}"
SOURCE_SHA256 = "3052855ec9bd7a8a549d3beef6f69e88bec8ca5d524774e71eeffde2d5141089"

LEVEL_DECLARED = ("3", "5", "5.1", "5.2", "6", "6.1", "6.2")  # written into l-base's SPS: the pictures stay the same
LEVEL_REFS = (8, 16)  # level 4.x holds 4 frames of 1080p, 5.0 holds 13, 5.1 and up 16
LEVEL_RATES = (("4.1", 30, 62.5), ("4.2", 60, 62.5), ("5", 60, 168.75))  # a level's 1080p frame rate and High's ceiling, Mbit/s
LEVEL_SIZES = ((1920, 1200), (1936, 1072), (1984, 768), (2032, 768), (2048, 768), (1872, 1120), (1904, 1104), (1080, 1920),
               (2048, 1080), (2560, 1440), (3840, 2160))  # past 1080p's 8160 macroblocks or 1920 pixels' width, or both
LEVEL_FPS = {f"l-rate{level.replace('.', '')}": fps for level, fps, _ in LEVEL_RATES}  # the rest are 30 fps

X264 = ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-profile:v", "high", "-f", "h264"]  # level: x264 picks the lowest that fits
X264_41 = ["-c:v", "libx264", "-profile:v", "high", "-level", "4.1", "-pix_fmt", "yuv420p", "-f", "h264"]


class MaterialError(RuntimeError):
    """A host tool is missing or failed while making the material."""


def sh(cmd: list[str], **kw) -> bytes:
    """Run @cmd and return its stdout; MaterialError with the tail of stderr when it fails."""
    if shutil.which(cmd[0]) is None:
        raise MaterialError(f"{cmd[0]} is not on PATH; the material needs ffmpeg and ffprobe")
    r = subprocess.run(cmd, capture_output=True, **kw)
    if r.returncode != 0:
        raise MaterialError(f"{cmd[0]} failed: {r.stderr.decode(errors='replace')[-400:]}")
    return r.stdout


def ffmpeg(*args: str, stdin: bytes | None = None) -> bytes:
    """ffmpeg with errors only and overwriting, as every recipe runs it."""
    return sh(["ffmpeg", "-v", "error", "-y", *args], input=stdin)


def cache_dir(root: Path | None = None) -> Path:
    """The material cache of this MATERIAL_VERSION under @root (the tests directory by default)."""
    return (root or TESTS) / ".cache" / f"material-v{MATERIAL_VERSION}"


def check_source(path: Path) -> None:
    """MaterialError unless @path holds the sample whose SHA-256 is SOURCE_SHA256."""
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != SOURCE_SHA256:
        raise MaterialError(f"{path}: SHA-256 {digest}, not the sample's {SOURCE_SHA256}; the clip and every baseline "
                            f"come from that file")


def source(cache: Path) -> Path:
    """The sample beside the material caches of @cache, downloaded when missing and checked every time."""
    path = cache.parent / "source" / SOURCE_NAME
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        try:
            with urllib.request.urlopen(SOURCE_URL, timeout=60) as r:
                data = r.read()
        except OSError as e:
            raise MaterialError(f"downloading {SOURCE_URL}: {e}; or put the file at {path} by hand") from None
        part = path.with_suffix(".part")
        part.write_bytes(data)
        check_source(part)
        part.rename(path)
    check_source(path)
    return path


# -- recipes: name -> how to make it; the name is the file (or directory) the recipe guarantees

RECIPES: dict[str, Callable[[Path], None]] = {}


def recipe(name: str, *needs: str) -> Callable:
    """Register the decorated function as the recipe of @name; it may rely on @needs being made first."""
    def wrap(fn):
        def make(cache: Path) -> None:
            for need in needs:
                ensure(need, cache)
            fn(cache)
        RECIPES[name] = make
        return fn
    return wrap


def ensure(name: str, cache: Path) -> Path:
    """The path of @name in @cache, made when missing; 'stills/<file>' names a still."""
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / name
    if name.startswith("stills/"):
        ensure("stills", cache)
        if not target.exists():
            raise MaterialError(f"{name}: no such still (stills.py makes none by that name)")
        return target
    if name not in RECIPES:
        raise MaterialError(f"{name}: no recipe")
    if not target.exists():
        RECIPES[name](cache)
    return target


def framemd5(cache: Path, name: str) -> None:
    """ffmpeg's per-frame digests of <name>.h264 as <name>.framemd5, when missing."""
    out = cache / f"{name}.framemd5"
    if not out.exists():
        out.write_bytes(ffmpeg("-i", str(cache / f"{name}.h264"), "-fps_mode", "passthrough", "-f", "framemd5", "-"))


@recipe("clip42.h264")
def _clip42(cache: Path) -> None:
    # the sample's own stream, its SPS's level 5.0 rewritten to 4.2, the decoder's: the pictures stay the same
    ffmpeg("-i", str(source(cache)), "-an", "-c:v", "copy", "-bsf:v", "h264_mp4toannexb,h264_metadata=level=4.2", "-f", "h264",
           str(cache / "clip42.h264"))
    framemd5(cache, "clip42")


RECIPES["clip42.framemd5"] = RECIPES["clip42.h264"]


@recipe("refs16.h264")
def _refs16(cache: Path) -> None:
    # 1080p with 16 reference frames, a decoder's most: 68 MiB of the codec heap
    ffmpeg("-i", str(source(cache)), "-an", "-c:v", "libx264", "-preset", "veryfast", "-profile:v", "high", "-level", "5.1", "-refs", "16",
           "-bf", "3", "-pix_fmt", "yuv420p", "-crf", "23", "-f", "h264", str(cache / "refs16.h264"))


def _clip42_input(cache: Path) -> list[str]:
    """The ffmpeg input arguments reading clip42.h264."""
    return ["-i", str(cache / "clip42.h264")]


def _raw_frame0(cache: Path) -> list[str]:
    """The ffmpeg input arguments reading frame0.i420."""
    return ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-i", str(cache / "frame0.i420")]


@recipe("frame0.i420", "clip42.h264")
def _frame0(cache: Path) -> None:
    ffmpeg(*_clip42_input(cache), "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "yuv420p", str(cache / "frame0.i420"))


@recipe("frame800.i420", "clip42.h264")
def _frame800(cache: Path) -> None:
    # rows of whole 32 pixels, not 64: the converter's input needs its own pitch
    ffmpeg(*_clip42_input(cache), "-frames:v", "1", "-vf", "scale=800:600", "-f", "rawvideo", "-pix_fmt", "yuv420p",
           str(cache / "frame800.i420"))


def _camera_picture(name: str, w: int, h: int, pix_fmt: str) -> None:
    """Register a camera's uncompressed picture: frame0 scaled, in the camera's packed layout (NV12's chroma right after its rows)."""
    @recipe(name, "frame0.i420")
    def make(cache: Path) -> None:
        ffmpeg(*_raw_frame0(cache), "-vf", f"scale={w}:{h}", "-f", "rawvideo", "-pix_fmt", pix_fmt, str(cache / name))


for _name, _w, _h, _fmt in (("frame640.yuyv", 640, 480, "yuyv422"), ("frame720.yuyv", 1280, 720, "yuyv422"),
                            ("frame640.nv12", 640, 480, "nv12"), ("frame1080.nv12", 1920, 1080, "nv12")):
    _camera_picture(_name, _w, _h, _fmt)

# frame0 at 640x480 in each format the converter is tried with (test_images.py): ffmpeg's own where it has one
for _name, _fmt in (("frame640.i420", "yuv420p"), ("frame640.nv21", "nv21"), ("frame640.i422", "yuv422p"),
                    ("frame640.uyvy", "uyvy422"), ("frame640.yvyu", "yvyu422"), ("frame640.rgbx", "rgb0"),
                    ("frame640.bgrx", "bgr0"), ("frame640.argb", "argb"), ("frame640.abgr", "abgr")):
    _camera_picture(_name, 640, 480, _fmt)


def vyuy_of_uyvy(data: bytes) -> bytes:
    """UYVY with U and V swapped: VYUY, which ffmpeg has no name for."""
    out = bytearray(data)
    out[0::4], out[2::4] = data[2::4], data[0::4]
    return bytes(out)


@recipe("frame640.vyuy", "frame640.uyvy")
def _frame640_vyuy(cache: Path) -> None:
    (cache / "frame640.vyuy").write_bytes(vyuy_of_uyvy((cache / "frame640.uyvy").read_bytes()))


COLUMN_ROW_BYTES = 128      # a column row's bytes
COLUMN10_SAMPLES = 96       # 10-bit samples a column row holds: three in each little-endian 32-bit word


def pack_columns10(y: list[int], u: list[int], v: list[int], w: int, h: int) -> bytes:
    """Planar 4:2:0 10-bit samples into VCF_NV12_10_COL128: per column of 96 samples, the luma rows (whole 16) then
    the interleaved Cb Cr rows, each row three samples to a little-endian 32-bit word, two top bits unused."""
    rows = (h + 15) & ~15
    lines = rows + rows // 2
    columns = (w + COLUMN10_SAMPLES - 1) // COLUMN10_SAMPLES
    out = bytearray(columns * COLUMN_ROW_BYTES * lines)

    def put(column: int, line: int, samples: list[int]) -> None:
        s = samples + [0] * (COLUMN10_SAMPLES - len(samples))
        words = [s[i] | (s[i + 1] << 10) | (s[i + 2] << 20) for i in range(0, COLUMN10_SAMPLES, 3)]
        at = (column * lines + line) * COLUMN_ROW_BYTES
        out[at:at + COLUMN_ROW_BYTES] = struct.pack(f"<{COLUMN10_SAMPLES // 3}I", *words)

    cw = w // 2
    for column in range(columns):
        x0 = column * COLUMN10_SAMPLES
        n = min(COLUMN10_SAMPLES, w - x0)
        for r in range(h):
            put(column, r, list(y[r * w + x0:r * w + x0 + n]))
        for r in range(h // 2):
            pairs = []
            for a, b in zip(u[r * cw + x0 // 2:r * cw + (x0 + n) // 2], v[r * cw + x0 // 2:r * cw + (x0 + n) // 2]):
                pairs += [a, b]
            put(column, rows + r, pairs)
    return bytes(out)


@recipe("frame640.y10c", "frame0.i420")
def _frame640_y10c(cache: Path) -> None:
    raw = ffmpeg(*_raw_frame0(cache), "-vf", "scale=640:480", "-f", "rawvideo", "-pix_fmt", "yuv420p10le", "-")
    n_y, n_c = 640 * 480, 320 * 240
    samples = struct.unpack(f"<{n_y + 2 * n_c}H", raw)
    (cache / "frame640.y10c").write_bytes(pack_columns10(list(samples[:n_y]), list(samples[n_y:n_y + n_c]),
                                                         list(samples[n_y + n_c:]), 640, 480))


@recipe("clip30.i420", "clip42.h264")
def _clip30(cache: Path) -> None:
    ffmpeg(*_clip42_input(cache), "-frames:v", "30", "-f", "rawvideo", "-pix_fmt", "yuv420p", str(cache / "clip30.i420"))


@recipe("pic.jpg", "frame0.i420")
def _pic(cache: Path) -> None:
    ffmpeg(*_raw_frame0(cache), "-q:v", "2", "-pix_fmt", "yuvj420p", str(cache / "pic.jpg"))


@recipe("pic640.jpg", "frame0.i420")
def _pic640(cache: Path) -> None:
    ffmpeg(*_raw_frame0(cache), "-vf", "scale=640:480", "-q:v", "2", "-pix_fmt", "yuvj420p", str(cache / "pic640.jpg"))


@recipe("prog.jpg", "pic.jpg")
def _prog(cache: Path) -> None:
    from PIL import Image
    Image.open(cache / "pic.jpg").save(cache / "prog.jpg", "JPEG", quality=90, progressive=True)


def _two_parts(cache: Path, name: str, part_a: list[str], part_b: list[str]) -> None:
    """Two streams joined, each with its own SPS/PPS, the second smaller: <name>.h264 and <name>.framemd5.

    Each part decodes on its own exactly as in the joined stream: their
    digests, joined, are the reference.
    """
    a, b = cache / f"{name}-a.h264", cache / f"{name}-b.h264"
    ffmpeg(*part_a, str(a))
    ffmpeg(*part_b, str(b))
    (cache / f"{name}.h264").write_bytes(a.read_bytes() + b.read_bytes())
    with open(cache / f"{name}.framemd5", "wb") as f:
        for part in (a, b):
            f.write(ffmpeg("-i", str(part), "-fps_mode", "passthrough", "-f", "framemd5", "-"))


@recipe("two.h264", "clip42.h264")
def _two(cache: Path) -> None:
    # the decoder meets a new shape at the seam
    _two_parts(cache, "two", [*_clip42_input(cache), "-frames:v", "60", *X264_41],
               [*_clip42_input(cache), "-frames:v", "60", "-vf", "scale=1280:720", *X264_41])


RECIPES["two.framemd5"] = RECIPES["two.h264"]


def _wide(cache: Path, name: str, sar: str) -> None:
    """720x576 of wide pixels, as a 16:9 TV picture is stored.

    64:45 (shown 1024x576) is written as an extended aspect, 16:11 (BT.601's,
    shown 1047x576) as one of H.264's predefined ones.
    """
    ffmpeg(*_clip42_input(cache), "-frames:v", "60", "-vf", f"scale=720:576,setsar={sar}", "-c:v", "libx264", "-profile:v",
           "high", "-level", "4.1", "-pix_fmt", "yuv420p", "-f", "h264", str(cache / name))


recipe("wide.h264", "clip42.h264")(lambda cache: _wide(cache, "wide.h264", "64/45"))
recipe("wide-idc.h264", "clip42.h264")(lambda cache: _wide(cache, "wide-idc.h264", "16/11"))


# -- the H.264 level probes, all from clip42 (30 fps); each has its framemd5 but the l-level ones

def _level_encode(cache: Path, name: str, frames: int, *opts: str) -> None:
    """One probe stream from clip42 with ffmpeg options @opts, and its digests."""
    ffmpeg("-r", "30", *_clip42_input(cache), "-frames:v", str(frames), *opts, str(cache / f"{name}.h264"))
    framemd5(cache, name)


def _level(name: str, frames: int, *opts: str) -> None:
    """Register the probe @name."""
    recipe(f"{name}.h264", "clip42.h264")(lambda cache: _level_encode(cache, name, frames, *opts))
    RECIPES[f"{name}.framemd5"] = RECIPES[f"{name}.h264"]


_level("l-base", 60, *X264, "-level", "4.1")
for _lvl in LEVEL_DECLARED:
    _name = "l-level" + _lvl.replace(".", "")
    recipe(f"{_name}.h264", "l-base.h264")(
        lambda cache, lvl=_lvl, name=_name: ffmpeg("-i", str(cache / "l-base.h264"), "-c", "copy", "-bsf:v",
                                                   f"h264_metadata=level={lvl}", "-f", "h264", str(cache / f"{name}.h264")))
for _refs in LEVEL_REFS:
    _level(f"l-ref{_refs}", 60, *X264, "-refs", str(_refs))
for _lvl, _fps, _mbit in LEVEL_RATES:  # 2 s of noise, so the encoder needs every bit it is allowed
    _level(f"l-rate{_lvl.replace('.', '')}", 2 * _fps, "-vf", f"fps={_fps},noise=alls=32:allf=t+u", *X264, "-level", _lvl,
           "-b:v", f"{_mbit}M", "-maxrate", f"{_mbit}M", "-bufsize", f"{_mbit}M")
for _w, _h in LEVEL_SIZES:
    _level(f"l-{_w}x{_h}", 30, "-vf", f"scale={_w}:{_h}", *X264)
# beyond High: any picture the decoder gives is wrong (its output is 8-bit 4:2:0), so their references never match
_level("l-high10", 30, "-c:v", "libx264", "-pix_fmt", "yuv420p10le", "-profile:v", "high10", "-f", "h264")
_level("l-high422", 30, "-c:v", "libx264", "-pix_fmt", "yuv422p", "-profile:v", "high422", "-f", "h264")


def describe(path: str | Path, fps: int = 30) -> str:
    """What the stream's first SPS declares: profile, level, size, reference frames; and its bit rate at @fps."""
    out = sh(["ffprobe", "-v", "error", "-select_streams", "v", "-count_frames", "-show_entries",
              "stream=profile,level,width,height,nb_read_frames", "-of", "default=nw=1", str(path)]).decode()
    f = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    r = subprocess.run(["ffmpeg", "-hide_banner", "-i", str(path), "-frames:v", "1", "-c", "copy", "-bsf:v", "trace_headers",
                        "-f", "null", "-"], capture_output=True, text=True)  # ffprobe's refs is not the SPS's
    refs = re.search(r"max_num_ref_frames\s+\S+ = (\d+)", r.stderr)
    mbit = os.path.getsize(path) * 8 * fps / int(f["nb_read_frames"]) / 1e6
    return (f"{f['profile']} level {int(f['level']) / 10:g}, {f['width']}x{f['height']}, {refs.group(1) if refs else '?'} refs, "
            f"{mbit:.1f} Mbit/s at {fps} fps")


# -- the still pictures (stills.py), a directory with a manifest

@recipe("stills", "pic.jpg")
def _stills(cache: Path) -> None:
    from .stills import make_stills
    out = cache / "stills"
    shutil.rmtree(out, ignore_errors=True)
    make_stills(str(out), str(cache / "pic.jpg"))


# -- the colour-bar pattern of the screenshot tests

PATTERN_X264 = ["-c:v", "libx264", "-preset", "medium", "-tune", "stillimage", "-pix_fmt", "yuv420p", "-profile:v", "high",
                "-level", "4.1", "-f", "h264"]


@recipe("pattern.png")
def _pattern_png(cache: Path) -> None:
    from PIL import Image
    from .pictures import pattern
    picture, _ = pattern()
    Image.fromarray(picture, "RGB").save(cache / "pattern.png")


@recipe("pattern.jpg", "pattern.png")
def _pattern_jpg(cache: Path) -> None:
    from PIL import Image
    Image.open(cache / "pattern.png").save(cache / "pattern.jpg", "JPEG", quality=90)


def _pattern_input(cache: Path) -> list[str]:
    """The ffmpeg input arguments showing pattern.png as a 30 fps stream."""
    return ["-loop", "1", "-framerate", "30", "-i", str(cache / "pattern.png")]


@recipe("pattern.h264", "pattern.png")
def _pattern_h264(cache: Path) -> None:
    # 60 identical frames: whatever frame is on screen at shot time, the bars are the same
    ffmpeg(*_pattern_input(cache), "-frames:v", "60", *PATTERN_X264, str(cache / "pattern.h264"))


@recipe("two-pattern.h264", "pattern.png")
def _two_pattern(cache: Path) -> None:
    # the bars at 1080p, then the same picture at 720p: the shape change, judged on screen
    _two_parts(cache, "two-pattern", [*_pattern_input(cache), "-frames:v", "60", *PATTERN_X264],
               [*_pattern_input(cache), "-frames:v", "60", "-vf", "scale=1280:720", *PATTERN_X264])


RECIPES["two-pattern.framemd5"] = RECIPES["two-pattern.h264"]


class Material:
    """The material of one run: made into @cache on first use, uploaded to the Amiga on first use."""

    def __init__(self, cache: Path, device=None):
        self.cache = cache
        self.device = device

    def path(self, name: str) -> Path:
        """The local file @name, made into the cache when missing."""
        return ensure(name, self.cache)

    def still_size(self, name: str) -> tuple[int, int]:
        """A still's (width, height) from the stills' manifest."""
        import json
        manifest = json.loads((ensure("stills", self.cache) / "manifest.json").read_text())
        w, h = manifest[Path(name).name]
        return w, h

    def ram(self, name: str) -> str:
        """The RAM:hw path of @name on the Amiga, uploaded once per boot (stills lose their directory)."""
        remote = f"RAM:hw/{Path(name).name}"
        self.device.upload_once(self.path(name), remote)
        return remote


def main(argv: list[str]) -> int:
    """Make every recipe (or those whose name contains --only) into the cache; print what was made."""
    import argparse
    parser = argparse.ArgumentParser(description="make the test material into the cache")
    parser.add_argument("--only", help="only the recipes whose name contains this")
    parser.add_argument("--cache", type=Path, default=cache_dir(), help="the cache directory")
    args = parser.parse_args(argv)
    start = time.time()
    made = 0
    for name in RECIPES:
        if args.only and args.only not in name:
            continue
        target = args.cache / name
        if target.exists():
            continue
        t0 = time.time()
        ensure(name, args.cache)
        print(f"{name}: {time.time() - t0:.1f} s")
        made += 1
    print(f"{made} made in {time.time() - start:.0f} s, cache {args.cache}")
    return 0


if __name__ == "__main__":
    if __package__ in (None, ""):  # run as a script: make the package importable
        sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
        from harness import material as _self  # noqa: F401
        sys.exit(_self.main(sys.argv[1:]))
    sys.exit(main(sys.argv[1:]))
