# SPDX-License-Identifier: BSD-3-Clause
"""
tools.py - what the tests share: the rigs' runs by name, the status checks,
stills through the image call and the datatype, the inputs decoded on the
Amiga, and the colour-bar judgement of screenshots.
"""
from __future__ import annotations

from pathlib import Path

import numpy as np

from .amiga import Device
from .material import Material
from .pictures import pattern, still_pixels
from .results import Result

MIB = 1 << 20
ZC_TROUBLE = (r"refused", r"never answered", r"never reported released", r"could not be freed", r"imports still held",
              r"matched nothing", r"SMEM service is gone", r"zero-copy buffer back at offset")


def vctest(device: Device, args: str, timeout: float = 120) -> Result:
    """VCTest with @args, run to its end."""
    return device.run("VCTest", args, timeout)


def vpuplay(device: Device, args: str, timeout: float = 120) -> Result:
    """VPUPlay with @args, run to its end."""
    return device.run("VPUPlay", args, timeout)


def status(device: Device) -> Result:
    """VCTest STATUS."""
    r = vctest(device, "STATUS", 30)
    assert r.passed, f"STATUS failed: {r.kv.get('fail')}"
    return r


def backend(r: Result, name: str = "videocore.codec") -> dict[str, str]:
    """The backend.N.* group of @name (the backend's file name) in a STATUS result."""
    for b in r.group("backend"):
        if b.get("name") == name:
            return b
    raise AssertionError(f"no backend {name} in STATUS: {r.group('backend')}")


def assert_clean(r: Result, services: int | None = None) -> None:
    """Nothing left behind: no session, no stray message on the codec; the services open as @services says."""
    b = backend(r)
    assert int(b["sessions"]) == 0, f"{b['sessions']} sessions still open"
    assert int(b["counter.stray"]) == 0, f"{b['counter.stray']} stray messages"
    if services is not None:
        assert r.num("vchiq.services") == services, f"{r.num('vchiq.services')} services open, {services} before"


def assert_nothing_left(device: Device) -> None:
    """The last test of a module: STATUS clean."""
    assert_clean(status(device))


def codec_memory(r: Result) -> tuple[int, int]:
    """(free, total) bytes of the codec heap from a STATUS result."""
    b = backend(r)
    return int(b["mem_free"]), int(b["mem_total"])


# -- inputs the Amiga makes itself

DERIVED = {  # name -> the VCTest DECODE arguments that write it from clip42
    "frame.i420": "FRAMES 1",
    "clip.i420": "FRAMES 30",
}


def derived(device: Device, material: Material, name: str) -> str:
    """RAM:hw/<name>, decoded from clip42 on the Amiga once per boot (a raw picture, or 30 of them)."""
    remote = f"RAM:hw/{name}"
    if remote not in device.ram_known:
        clip = material.ram("clip42.h264")
        r = vctest(device, f"DECODE {clip} {DERIVED[name]} OUT {remote} NOHASH", 120)
        assert r.passed, f"making {name}: {r.kv.get('fail')}"
        device.ram_known.add(remote)
    return remote


# -- stills

def still_decode(device: Device, material: Material, name: str, fmt: str = "RGBA32", size: tuple[int, int] | None = None,
                 extra: str = "", fetch: bool = True, misalign: bool = False) -> tuple[int, int, Result, bytes | None]:
    """VCTest IMAGE of a still: (VC_DecodeImage's code, its microseconds, the result, the picture's bytes or None)."""
    remote = material.ram(f"stills/{name}")
    out = "RAM:hw/still.out"
    sized = f" OUTWIDTH {size[0]} OUTHEIGHT {size[1]}" if size else ""
    r = vctest(device, f"IMAGE {remote} OUT {out} OUTFORMAT {fmt}{sized} {extra}{' MISALIGN' if misalign else ''}", 120)
    code = int(r.num("image.rc", 99)) if r.has("image.rc") else int(r.num("header.rc", 99))
    us = int(r.num("image.us", 0))
    raw = device.download(out) if code == 0 and fetch else None
    device.delete(out)
    return code, us, r, raw


def still_time(device: Device, material: Material, name: str, fmt: str, misalign: bool = False, times: int = 3) -> int:
    """The image call's microseconds for a still, the best of @times runs (the first call of a program pays for setup)."""
    best = None
    for _ in range(times):
        code, us, _, _ = still_decode(device, material, name, fmt, fetch=False, misalign=misalign)
        assert code == 0, f"VC_DecodeImage {code}"
        best = us if best is None else min(best, us)
    return best


def still_reference(material: Material, name: str) -> np.ndarray:
    """Pillow's decode of a still as RGBA rows (stills.py wrote it)."""
    w, h = material.still_size(name)
    data = material.path(f"stills/{Path(name).name}.rgba").read_bytes()
    return np.frombuffer(data, np.uint8).reshape(h, w, 4)


def still_rgb(fmt: str, raw: bytes, w: int, h: int) -> tuple[np.ndarray, np.ndarray | None]:
    """An output picture as RGB rows and its alpha."""
    return still_pixels(fmt, w, h, raw)


def dt_load(device: Device, material: Material, name: str, out: bool = False) -> tuple[str | None, int | None, int, str | None, bytes | None]:
    """VCTest DATATYPE of a still: (descriptor, NewDTObject's code, microseconds, the pixels' sum, the PPM's bytes or None)."""
    remote = material.ram(f"stills/{name}")
    ppm = "RAM:hw/dt.ppm"
    r = vctest(device, f"DATATYPE {remote}" + (f" OUT {ppm}" if out else ""), 180)
    desc = r.kv.get("datatype.descriptor")
    code = int(r.num("datatype.rc")) if r.has("datatype.rc") else None
    us = int(r.num("datatype.us", 0))
    total = r.kv.get("datatype.sum")
    raw = device.download(ppm) if out and code == 0 else None
    device.delete(ppm)
    return desc, code, us, total, raw


def dt_page(device: Device, material: Material, names: tuple[str, ...], copies: int) -> Result:
    """VCTest PAGE: @copies of each still loaded at once, a process each."""
    files = " ".join(material.ram(f"stills/{name}") for name in names)
    return vctest(device, f"PAGE {files} COPIES {copies}", 180)


# -- the colour bars on screen

def fitted(width: int, height: int) -> tuple[int, int, int, int]:
    """Where a 16:9 picture lands in a @width x @height area with its aspect kept, centred: (x, y, w, h)."""
    w = min(width, height * 16 // 9)
    h = w * 9 // 16
    return (width - w) // 2, (height - h) // 2, w, h


def picture_bars(x: int, y: int, w: int, h: int) -> list:
    """The pattern's bars scaled into the picture at (@x, @y) of @w x @h on screen."""
    _, bars = pattern()
    sx, sy = w / 1920, h / 1080
    return [type(b)(x + b.x * sx, y + b.y * sy, b.w * sx, b.h * sy, b.rgb) for b in bars]


def limited(rgb: tuple) -> np.ndarray:
    """A colour as a limited-range video path shows it: the VPU's H.264 output keeps BT.601's 16 to 235 (a JPEG's is full)."""
    return 16 + np.array(rgb, np.float64) * 219 / 255


def judge_screen_bars(device: Device, bars: list, name: str, tolerance: float = 8, max_std: float = 6) -> list[str]:
    """Sample a patch at each bar's centre on the frontmost screen; the problems in plain words (empty: fine).

    A bar passes at its own colour or at that colour's limited-range rendering, whichever is nearer.
    """
    device.wake()  # the screen blanker would be in front
    problems = []
    for i, b in enumerate(bars):
        size = int(max(8, min(48, b.w * 0.4, b.h * 0.4)))
        cx, cy = int(b.x + b.w / 2), int(b.y + b.h * 0.4)
        patch = device.screenshot((cx - size // 2, cy - size // 2, size, size), name=f"{name}-bar{i}").astype(np.float64)
        mean = patch.reshape(-1, 3).mean(axis=0)
        std = patch.reshape(-1, 3).std(axis=0).max()
        off = min(np.abs(mean - np.array(b.rgb)).max(), np.abs(mean - limited(b.rgb)).max())
        if off > tolerance:
            problems.append(f"bar {i} at {cx},{cy}: mean {mean.round(1).tolist()}, wanted {list(b.rgb)} "
                            f"(or {limited(b.rgb).round(0).astype(int).tolist()} limited)")
        elif std > max_std:
            problems.append(f"bar {i} at {cx},{cy}: not flat (std {std:.1f})")
    return problems
