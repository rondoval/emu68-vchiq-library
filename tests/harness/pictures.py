# SPDX-License-Identifier: BSD-3-Clause
"""
pictures.py - the picture maths of the tests: loaders for what the rigs
write (PPM, raw RGB565, I420), PSNR against ffmpeg's decode under each colour
reading the VPU might use, the H.264 key-frame list, and the colour-bar
pattern the screenshot tests show and judge.

Moved from the former testdata/hwcycle.py (load_ppm, psnr, ref_rgb, best_psnr,
rgb565_picture, y_planes, key_frames, still_pixels); pattern(), bar_means()
and judge_bars() are new.
"""
from __future__ import annotations

import re
from collections import namedtuple
from pathlib import Path

import numpy as np

Bar = namedtuple("Bar", "x y w h rgb")

BAR_COLOURS = ((255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
               (255, 0, 255), (255, 0, 0), (0, 0, 255), (16, 16, 16))


def load_ppm(path: str | Path) -> np.ndarray:
    """A binary PPM (P6, comments allowed) as an HxWx3 uint8 array."""
    data = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:
        m = re.compile(rb"\s*(#[^\n]*\n\s*)*(\S+)").match(data, pos)
        fields.append(m.group(2))
        pos = m.end()
    w, h = int(fields[1]), int(fields[2])
    return np.frombuffer(data[pos + 1:pos + 1 + w * h * 3], np.uint8).reshape(h, w, 3)


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    """PSNR of @a against @b in dB; 99.0 for identical arrays."""
    a = a.astype(np.float64)
    b = b.astype(np.float64)
    if a.shape != b.shape:
        raise AssertionError(f"size {a.shape} against the reference's {b.shape}")
    mse = np.mean((a - b) ** 2)
    return 99.0 if mse == 0 else float(10 * np.log10(255.0 * 255.0 / mse))


def ref_rgb(src_args: list[str], w: int, h: int, pix_fmt: str = "rgb24") -> dict[str, np.ndarray]:
    """The reference picture under each colour reading the VPU might use: {name: array}."""
    from .material import ffmpeg
    out = {}
    for matrix in ("bt601", "bt709"):
        for rng in ("limited", "full"):
            vf = f"scale={w}:{h}:flags=bicubic:in_color_matrix={matrix}:in_range={rng}"
            data = ffmpeg(*src_args, "-vf", vf, "-f", "rawvideo", "-pix_fmt", pix_fmt, "-")
            if pix_fmt == "rgb565le":
                arr = rgb565_pixels(data, w, h)
            else:
                arr = np.frombuffer(data, np.uint8).reshape(h, w, 3)
            out[f"{matrix}/{rng}"] = arr
    return out


def best_psnr(picture: np.ndarray, refs: dict[str, np.ndarray]) -> tuple[float, str]:
    """The best PSNR of @picture over the references and which reading gave it."""
    scores = {name: psnr(picture, ref) for name, ref in refs.items()}
    name = max(scores, key=scores.get)
    return scores[name], name


def rgb565_pixels(data: bytes, w: int, h: int) -> np.ndarray:
    """Little-endian RGB565 pixels as an HxWx3 uint8 array (the low bits zero)."""
    v = np.frombuffer(data[: w * h * 2], "<u2").reshape(h, w)
    return np.stack([((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3], -1).astype(np.uint8)


def rgb565_picture(path: str | Path, w: int, h: int) -> np.ndarray:
    """A raw RGB565 file as an HxWx3 uint8 array."""
    return rgb565_pixels(open(path, "rb").read(), w, h)


def y_planes(path_or_bytes: str | Path | bytes, w: int, h: int, frames: int) -> list[np.ndarray]:
    """The luma planes of @frames I420 pictures, from a file or bytes."""
    data = path_or_bytes if isinstance(path_or_bytes, bytes) else open(path_or_bytes, "rb").read()
    size = w * h * 3 // 2
    return [np.frombuffer(data[i * size: i * size + w * h], np.uint8).reshape(h, w) for i in range(frames)]


def key_frames(path: str | Path) -> tuple[int, list[int]]:
    """How many frames an H.264 file has and which of them are key frames (ffprobe)."""
    from .material import sh
    out = sh(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "frame=key_frame", "-of", "csv=p=0", str(path)])
    flags = [line.strip() for line in out.decode().splitlines() if line.strip()]
    return len(flags), [i for i, f in enumerate(flags) if f.startswith("1")]


def still_pixels(fmt: str, w: int, h: int, raw: bytes) -> tuple[np.ndarray, np.ndarray | None]:
    """A rig's output picture as RGB rows and its alpha (None for 24-bit), in red-green-blue order."""
    if fmt == "RGB24":
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\n", raw)
        return np.frombuffer(raw[m.end():m.end() + w * h * 3], np.uint8).reshape(h, w, 3), None
    if fmt == "BGR24":
        return np.frombuffer(raw[:w * h * 3], np.uint8).reshape(h, w, 3)[:, :, ::-1], None
    px = np.frombuffer(raw[:w * h * 4], np.uint8).reshape(h, w, 4)
    return (px[:, :, :3] if fmt == "RGBA32" else px[:, :, [2, 1, 0]]), px[:, :, 3]


def pattern(width: int = 1920, height: int = 1080) -> tuple[np.ndarray, list[Bar]]:
    """The colour-bar picture: eight bars over the top three quarters, a grey ramp below.

    Every bar edge lies on a multiple of 16 pixels (the bar height is rounded
    down to one), so a 4:2:0 encode and a 16-pixel macroblock grid keep the
    edges where they are.
    """
    bar_w = width // 8 // 16 * 16
    bar_h = height * 3 // 4 // 16 * 16
    picture = np.zeros((height, width, 3), np.uint8)
    bars = []
    for i, rgb in enumerate(BAR_COLOURS):
        x = i * bar_w
        picture[:bar_h, x:x + bar_w] = rgb
        bars.append(Bar(x, 0, bar_w, bar_h, rgb))
    picture[:bar_h, 8 * bar_w:] = BAR_COLOURS[-1]  # the remainder, if the width is no multiple of 128
    ramp = (np.arange(width) * 255 // max(width - 1, 1)).astype(np.uint8)
    picture[bar_h:] = ramp[None, :, None]
    return picture, bars


def raw_bars(fmt: str, width: int = 1920, height: int = 1080) -> bytes:
    """pattern() as ffmpeg's rawvideo in a VCF_* layout."""
    rgb = pattern(width, height)[0]
    alpha = np.full((height, width, 1), 255, np.uint8)
    if fmt == "RGB24":
        return rgb.tobytes()
    if fmt == "BGR24":
        return rgb[..., ::-1].tobytes()
    if fmt == "RGBA32":
        return np.concatenate((rgb, alpha), axis=2).tobytes()
    if fmt == "BGRA32":
        return np.concatenate((rgb[..., ::-1], alpha), axis=2).tobytes()
    r, g, b = (rgb[..., i].astype(np.uint16) for i in range(3))
    return (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)).astype("<u2").tobytes()


def bar_means(shot: np.ndarray, bars: list[Bar], scale: float = 1.0,
              origin: tuple[int, int] = (0, 0)) -> list[tuple[tuple[float, float, float], float]]:
    """Per bar, the mean RGB of its inner 80 % and the largest per-channel standard deviation.

    @bars are in pattern coordinates; @scale and @origin place them on the
    screenshot (shot = origin + scale * pattern).
    """
    out = []
    for bar in bars:
        x, y = origin[0] + bar.x * scale, origin[1] + bar.y * scale
        w, h = bar.w * scale, bar.h * scale
        x0, x1 = int(round(x + w / 10)), int(round(x + w - w / 10))
        y0, y1 = int(round(y + h / 10)), int(round(y + h - h / 10))
        region = shot[y0:y1, x0:x1, :3].astype(np.float64)
        mean = tuple(float(v) for v in region.reshape(-1, 3).mean(axis=0))
        std = float(region.reshape(-1, 3).std(axis=0).max())
        out.append((mean, std))
    return out


def judge_bars(shot: np.ndarray, bars: list[Bar], tolerance: float = 8, max_std: float = 6,
               scale: float = 1.0, origin: tuple[int, int] = (0, 0)) -> list[str]:
    """What is wrong with the bars on @shot, in plain words; an empty list when they are right."""
    problems = []
    names = ("red", "green", "blue")
    for i, (bar, (mean, std)) in enumerate(zip(bars, bar_means(shot, bars, scale, origin))):
        off = [(names[c], mean[c] - bar.rgb[c]) for c in range(3) if abs(mean[c] - bar.rgb[c]) > tolerance]
        if off:
            where = ", ".join(f"{name} off by {delta:+.0f}" for name, delta in off)
            problems.append(f"bar {i} {bar.rgb}: mean ({mean[0]:.0f}, {mean[1]:.0f}, {mean[2]:.0f}), {where}")
        if std > max_std:
            problems.append(f"bar {i} {bar.rgb}: not flat, standard deviation {std:.1f}")
    return problems
