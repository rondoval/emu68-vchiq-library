# SPDX-License-Identifier: BSD-3-Clause
"""The pixel formats beyond the ISP's six (docs/firmware-facts.md, "ISP formats"): each the converter offers, as its
input and as its output through VC_ConvertImage(), against ffmpeg's conversion of the same picture; the rest refused."""
import numpy as np
import pytest

from harness.material import ffmpeg, vyuy_of_uyvy
from harness.pictures import best_psnr, load_ppm, ref_rgb
from harness.tools import vctest

# The formats the converter reads beyond the ISP's own six and a camera's YUYV and NV12 (tests/test_images.py), each
# with its picture: frame0 at 640x480 in that layout (the harness's material)
INPUTS = {"UYVY": "frame640.uyvy", "NV21": "frame640.nv21", "I422": "frame640.i422", "YVYU": "frame640.yvyu",
          "VYUY": "frame640.vyuy", "RGBX32": "frame640.rgbx", "BGRX32": "frame640.bgrx"}
# What it writes besides the six: ffmpeg's name for the layout of a YUV one, the channel order of a 32-bit RGB one
YUV_OUTPUTS = {"YV12": "yuv420p", "NV12": "nv12", "YUYV": "yuyv422", "UYVY": "uyvy422", "NV21": "nv21", "I422": "yuv422p",
               "YVYU": "yvyu422", "VYUY": "uyvy422"}
RGB_OUTPUTS = {"RGBX32": ((0, 1, 2), 3), "BGRX32": ((2, 1, 0), 3)}
# What it must refuse, VCERR_UNSUPPORTED: the ISP refuses them, reads them wrong (YV12's chroma planes swapped), or has no
# encoding for them (the Amiga's own screen formats); docs/firmware-facts.md
NOT_READ = ("YV12", "ARGB32", "ABGR32", "NV12_10_COL128", "RGB16BE", "RGB15", "RGB15BE", "CLUT8")
NOT_WRITTEN = ("ARGB32", "ABGR32", "NV12_10_COL128", "RGB16BE", "RGB15", "RGB15BE", "CLUT8")
ANY_PICTURE = "frame640.argb"  # bytes enough for any of NOT_READ at 640x480: they are refused before anything reads them
UNSUPPORTED = "-6"
LEAST_DB = 30.0  # well under what a right reading gives (45 dB and more), well over a wrong one (11.8 dB, chroma swapped)
IN_W, IN_H = 640, 480
OUT_W, OUT_H = 800, 600


def frame0_src(material):
    """ffmpeg's input arguments for frame0, which every input picture was made from."""
    return ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-i", str(material.path("frame0.i420"))]


def frame800_src(material):
    """ffmpeg's input arguments for frame800.i420, the outputs' source."""
    return ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", f"{OUT_W}x{OUT_H}", "-i", str(material.path("frame800.i420"))]


def rgb_of(data: bytes, fmt: str, w: int, h: int) -> tuple[np.ndarray, np.ndarray | None]:
    """A converted picture as RGB, read the ISP's way (BT.601 full range) where it is YUV; and its fourth bytes."""
    if fmt in YUV_OUTPUTS:
        if fmt == "VYUY":
            data = vyuy_of_uyvy(data)  # its own inverse: back to UYVY
        out = ffmpeg("-f", "rawvideo", "-pix_fmt", YUV_OUTPUTS[fmt], "-s", f"{w}x{h}", "-i", "-", "-vf",
                     "scale=in_color_matrix=bt601:in_range=full", "-f", "rawvideo", "-pix_fmt", "rgb24", "-", stdin=data)
        return np.frombuffer(out, np.uint8).reshape(h, w, 3), None
    order, extra = RGB_OUTPUTS[fmt]
    quads = np.frombuffer(data, np.uint8).reshape(h, w, 4)
    return quads[:, :, list(order)], quads[:, :, extra]


@pytest.mark.parametrize("fmt", sorted(INPUTS))
def test_input(device, material, artifacts, record_property, fmt):
    """The converter reads @fmt: 640x480 to RGB24, as ffmpeg reads the picture it was made from."""
    r = vctest(device, f"CONVERT {material.ram(INPUTS[fmt])} WIDTH {IN_W} HEIGHT {IN_H} FORMAT {fmt} OUTFORMAT RGB24 "
                       "OUT RAM:hw/format.ppm", 60)
    assert r.passed, f"VC_ConvertImage {r.kv.get('convert.rc')}: {r.kv.get('fail')}"
    data = device.download("RAM:hw/format.ppm")
    (artifacts / f"in-{fmt}.ppm").write_bytes(data)
    device.delete("RAM:hw/format.ppm")
    score, how = best_psnr(load_ppm(artifacts / f"in-{fmt}.ppm"), ref_rgb(frame0_src(material), IN_W, IN_H))
    record_property("psnr", f"{score:.1f} dB as {how}")
    assert score >= LEAST_DB, f"{score:.1f} dB ({how}): {fmt} read wrong"


@pytest.mark.parametrize("fmt", sorted({*YUV_OUTPUTS, *RGB_OUTPUTS}))
def test_output(device, material, artifacts, record_property, fmt):
    """The converter writes @fmt: 800x600 from I420, as ffmpeg reads the source the ISP's way."""
    r = vctest(device, f"CONVERT {material.ram('frame800.i420')} WIDTH {OUT_W} HEIGHT {OUT_H} OUTFORMAT {fmt} "
                       "OUT RAM:hw/format.raw", 60)
    assert r.passed, f"VC_ConvertImage {r.kv.get('convert.rc')}: {r.kv.get('fail')}"
    data = device.download("RAM:hw/format.raw")
    (artifacts / f"out-{fmt}.raw").write_bytes(data)
    device.delete("RAM:hw/format.raw")
    picture, fourth = rgb_of(data, fmt, OUT_W, OUT_H)
    score, how = best_psnr(picture, ref_rgb(frame800_src(material), OUT_W, OUT_H))
    record_property("psnr", f"{score:.1f} dB as {how}")
    if fourth is not None:
        record_property("fourth_byte", f"{int(fourth.min())}..{int(fourth.max())}")
    assert score >= LEAST_DB, f"{score:.1f} dB ({how}): {fmt} written wrong"


@pytest.mark.parametrize("fmt", NOT_READ)
def test_input_refused(device, material, fmt):
    """A format the converter does not read right is refused, VCERR_UNSUPPORTED, not converted wrong."""
    r = vctest(device, f"CONVERT {material.ram(ANY_PICTURE)} WIDTH {IN_W} HEIGHT {IN_H} FORMAT {fmt} OUTFORMAT RGB24 "
                       "OUT RAM:hw/format.ppm", 60)
    assert not r.passed and r.kv.get("convert.rc") == UNSUPPORTED, f"rc {r.rc}, convert.rc {r.kv.get('convert.rc')}"


@pytest.mark.parametrize("fmt", NOT_WRITTEN)
def test_output_refused(device, material, fmt):
    """A format the converter does not write is refused, VCERR_UNSUPPORTED, not converted wrong."""
    r = vctest(device, f"CONVERT {material.ram('frame800.i420')} WIDTH {OUT_W} HEIGHT {OUT_H} OUTFORMAT {fmt} "
                       "OUT RAM:hw/format.raw", 60)
    assert not r.passed and r.kv.get("convert.rc") == UNSUPPORTED, f"rc {r.rc}, convert.rc {r.kv.get('convert.rc')}"
