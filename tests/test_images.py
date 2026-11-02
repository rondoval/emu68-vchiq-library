# SPDX-License-Identifier: BSD-3-Clause
"""The image calls: JPEG decode, scaling, conversion, JPEG encode, the still formats, refusals, headers; VCShow the example."""
import time

import numpy as np
import pytest

from harness.material import ffmpeg
from harness.pictures import best_psnr, load_ppm, psnr, ref_rgb, rgb565_pixels, y_planes
from harness.results import expect
from harness.tools import (assert_clean, assert_nothing_left, derived, judge_screen_bars, picture_bars, status, still_decode,
                           still_reference, still_rgb, still_time, vctest)


def fetch_ppm(device, remote, artifacts, name):
    """A PPM the rig wrote, saved in the artifacts and loaded."""
    data = device.download(remote)
    (artifacts / name).write_bytes(data)
    device.delete(remote)
    return load_ppm(artifacts / name)


@pytest.mark.smoke
def test_jpeg_decode(device, material, baselines, artifacts):
    """VC_DecodeImage of a 1920x1080 baseline JPEG, against ffmpeg's decode; its time."""
    r = vctest(device, f"IMAGE {material.ram('pic.jpg')} OUT RAM:hw/pic.ppm")
    assert r.passed, r.kv.get("fail")
    pic = fetch_ppm(device, "RAM:hw/pic.ppm", artifacts, "pic.ppm")
    score, how = best_psnr(pic, ref_rgb(["-i", str(material.path("pic.jpg"))], 1920, 1080))
    baselines.check("jpeg_1080p.psnr_db", round(score, 1), higher=True, tolerance=2.0)
    us = min(int(r.num("image.us")), still_time(device, material, "j1080.jpg", "RGB24", times=2))
    baselines.check("jpeg_1080p.image.us", us, higher=False, tolerance="25%")


def test_jpeg_size_hint(device, material, baselines, artifacts):
    """A 640x480 JPEG, its size also the call's hint: the picture must not be lost (-8)."""
    r = vctest(device, f"IMAGE {material.ram('pic640.jpg')} OUT RAM:hw/pic640.ppm")
    assert r.passed, f"VC_DecodeImage {r.kv.get('image.rc')}: {r.kv.get('fail')}"
    pic = fetch_ppm(device, "RAM:hw/pic640.ppm", artifacts, "pic640.ppm")
    score, _ = best_psnr(pic, ref_rgb(["-i", str(material.path("pic640.jpg"))], 640, 480))
    baselines.check("jpeg_640.psnr_db", round(score, 1), higher=True, tolerance=2.0)


def test_jpeg_scaled(device, material, baselines, artifacts):
    """VC_DecodeImage scaled to 320x240 on the way."""
    r = vctest(device, f"IMAGE {material.ram('pic.jpg')} OUT RAM:hw/small.ppm OUTWIDTH 320 OUTHEIGHT 240")
    assert r.passed, r.kv.get("fail")
    pic = fetch_ppm(device, "RAM:hw/small.ppm", artifacts, "small.ppm")
    score, _ = best_psnr(pic, ref_rgb(["-i", str(material.path("pic.jpg"))], 320, 240))
    baselines.check("jpeg_scaled_320.psnr_db", round(score, 1), higher=True, tolerance=2.0)


def frame0_src(material):
    """ffmpeg's input arguments for the host's copy of clip42's first frame."""
    return ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-i", str(material.path("frame0.i420"))]


@pytest.mark.smoke
def test_convert_rgb16_720p(device, material, baselines, artifacts):
    """VC_ConvertImage: I420 1080p to RGB16 720p, against ffmpeg's."""
    frame = derived(device, material, "frame.i420")
    r = vctest(device, f"CONVERT {frame} WIDTH 1920 HEIGHT 1080 OUTWIDTH 1280 OUTHEIGHT 720 OUTFORMAT RGB16 OUT RAM:hw/c720.rgb")
    assert r.passed, r.kv.get("fail")
    data = device.download("RAM:hw/c720.rgb")
    device.delete("RAM:hw/c720.rgb")
    score, _ = best_psnr(rgb565_pixels(data, 1280, 720), ref_rgb(frame0_src(material), 1280, 720, "rgb565le"))
    baselines.check("convert_720p_rgb16.psnr_db", round(score, 1), higher=True, tolerance=2.0)


@pytest.mark.parametrize("size", [(320, 180), (3840, 2160)])
def test_convert_limits(device, material, baselines, artifacts, size):
    """The scaler's limits: 320x180 and 3840x2160 RGB24; a refusal is a fact of the hardware, pinned by the baseline."""
    w, h = size
    frame = derived(device, material, "frame.i420")
    r = vctest(device, f"CONVERT {frame} WIDTH 1920 HEIGHT 1080 OUTWIDTH {w} OUTHEIGHT {h} OUTFORMAT RGB24 OUT RAM:hw/c{w}.ppm", 120)
    rc = int(r.num("convert.rc"))
    baselines.check(f"convert_{w}x{h}.convert.rc", rc, higher=True, tolerance=0)
    if rc == 0:
        pic = fetch_ppm(device, f"RAM:hw/c{w}.ppm", artifacts, f"c{w}.ppm")
        score, _ = best_psnr(pic, ref_rgb(frame0_src(material), w, h))
        baselines.check(f"convert_{w}x{h}.psnr_db", round(score, 1), higher=True, tolerance=2.0)


def test_convert_800_rows(device, material, baselines, artifacts):
    """An I420 picture 800 pixels wide (rows of whole 32, not 64) to RGB24: the converter's own input pitch."""
    r = vctest(device, f"CONVERT {material.ram('frame800.i420')} WIDTH 800 HEIGHT 600 OUTFORMAT RGB24 OUT RAM:hw/c800.ppm")
    assert r.passed, r.kv.get("fail")
    pic = fetch_ppm(device, "RAM:hw/c800.ppm", artifacts, "c800.ppm")
    src = ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "800x600", "-i", str(material.path("frame800.i420"))]
    score, _ = best_psnr(pic, ref_rgb(src, 800, 600))
    baselines.check("convert_800x600.psnr_db", round(score, 1), higher=True, tolerance=2.0)


CAMERA_PIX_FMT = {"YUYV": "yuyv422", "NV12": "nv12"}


def camera_src(material, name, w, h, fmt):
    """ffmpeg's input arguments for the host's copy of a camera's picture."""
    return ["-f", "rawvideo", "-pix_fmt", CAMERA_PIX_FMT[fmt], "-s", f"{w}x{h}", "-i", str(material.path(name))]


@pytest.mark.parametrize("name, w, h, fmt", [("frame640.yuyv", 640, 480, "YUYV"), ("frame640.nv12", 640, 480, "NV12"),
                                             ("frame1080.nv12", 1920, 1080, "NV12")], ids=["yuyv640", "nv12-640", "nv12-1080"])
def test_convert_camera_formats(device, material, baselines, artifacts, name, w, h, fmt):
    """A camera's YUYV and NV12 to RGB24, NV12's chroma right after its 1080 rows as a camera sends it; read as full-range BT.601, the firmware's only reading."""
    r = vctest(device, f"CONVERT {material.ram(name)} WIDTH {w} HEIGHT {h} FORMAT {fmt} OUTFORMAT RGB24 OUT RAM:hw/cam.ppm", 60)
    assert r.passed, f"VC_ConvertImage {r.kv.get('convert.rc')}: {r.kv.get('fail')}"
    pic = fetch_ppm(device, "RAM:hw/cam.ppm", artifacts, f"{name}.ppm")
    score, how = best_psnr(pic, ref_rgb(camera_src(material, name, w, h, fmt), w, h))
    assert how == "bt601/full", f"read as {how} ({score:.1f} dB): the firmware's reading changed (docs/firmware-facts.md)"
    baselines.check(f"convert_{name}.psnr_db", round(score, 1), higher=True, tolerance=2.0)


@pytest.mark.parametrize("name, w, h", [("frame640.yuyv", 640, 480), ("frame720.yuyv", 1280, 720)], ids=["640x480", "1280x720"])
def test_converter_session_rate(device, material, baselines, artifacts, record_property, name, w, h):
    """A camera's YUYV through a converter session into a screen's BGRA32, 150 times: at least 30 a second, what a preview needs."""
    r = vctest(device, f"CONVERT {material.ram(name)} WIDTH {w} HEIGHT {h} FORMAT YUYV OUTFORMAT BGRA32 COUNT 150 OUT RAM:hw/rate.raw", 120)
    assert r.passed, f"{r.kv.get('fail')}, error {r.kv.get('error')}"
    device.delete("RAM:hw/rate.raw")
    fps = r.num("convert.fps")
    record_property("rate", f"{fps} pictures a second, {r.num('convert.per_picture_us')} us each, open {r.num('convert.open_us')} us")
    assert r.num("convert.pictures") == 150, f"{r.kv.get('convert.pictures')} pictures"
    assert fps >= 30, f"{fps} pictures a second"
    baselines.check(f"converter_{w}x{h}.fps", fps, higher=True, tolerance="20%")


def test_converter_refuses_unlisted(device, material):
    """An input format the converter does not name (UYVY) is refused at the open, VCERR_UNSUPPORTED, not left to the firmware."""
    r = vctest(device, f"CONVERT {material.ram('frame640.yuyv')} WIDTH 640 HEIGHT 480 FORMAT UYVY OUTFORMAT RGB24 COUNT 1 OUT RAM:hw/x.ppm")
    assert not r.passed and r.kv.get("convert.session") == "refused", f"session {r.kv.get('convert.session')}: {r.kv.get('fail')}"
    assert r.num("error") == -6, f"error {r.kv.get('error')}, not VCERR_UNSUPPORTED"


@pytest.mark.smoke
def test_jpeg_encode(device, material, baselines, artifacts, record_property):
    """VC_EncodeImage: a 1080p frame to a JPEG at quality 80, which ffmpeg decodes back close to the frame."""
    frame = derived(device, material, "frame.i420")
    r = vctest(device, f"TOJPEG {frame} WIDTH 1920 HEIGHT 1080 QUALITY 80 OUT RAM:hw/frame.jpg")
    assert r.passed, f"VC_EncodeImage {r.kv.get('encode_image.rc')}: {r.kv.get('fail')}"
    jpg = artifacts / "frame.jpg"
    jpg.write_bytes(device.download("RAM:hw/frame.jpg"))
    device.delete("RAM:hw/frame.jpg")
    got = y_planes(ffmpeg("-i", str(jpg), "-f", "rawvideo", "-pix_fmt", "yuvj420p", "-"), 1920, 1080, 1)[0]
    ref = y_planes(material.path("frame0.i420"), 1920, 1080, 1)[0]
    record_property("bytes", r.kv.get("encode_image.bytes"))
    baselines.check("jpeg_encode_q80.psnr_db", round(psnr(got, ref), 1), higher=True, tolerance=2.0)


def test_progressive_refused(device, material):
    """A progressive JPEG is refused (or given up at the call's timeout), no hang, nothing left behind."""
    r = vctest(device, f"IMAGE {material.ram('prog.jpg')} OUT RAM:hw/prog.ppm", 40)
    assert r.rc == 20 and r.num("image.rc") in (-6, -7, -8), f"VC_DecodeImage {r.kv.get('image.rc')}"
    assert_clean(status(device))


# -- the still formats through the image decoder

STILL_NATIVE = [  # name, VCTest extras, least PSNR against Pillow (99: exact), alpha compared
    ("j420.jpg", "", 45, True), ("j422.jpg", "", 45, True), ("j444.jpg", "", 45, True), ("jgrey.jpg", "", 45, True),
    ("jrst.jpg", "", 45, True), ("jmeta79.jpg", "", 45, True), ("jbigmeta.jpg", "", 45, True), ("prgb.png", "", 99, True), ("prgba.png", "", 99, True),
    ("ppal.png", "", 99, True), ("palpha.png", "", 99, True),
    ("gstatic.gif", "", 38, True), ("ganim.gif", "", 38, True), ("b24.bmp", "", 99, True), ("b32.bmp", "", 99, False),
    ("b8.bmp", "", 99, True), ("btop.bmp", "", 99, True), ("t24.tga", "ENCODING TGA", 99, True), ("t32.tga", "ENCODING TGA", 99, True),
    ("p6.ppm", "", 99, True), ("p5.pgm", "", 99, True),
]
STILL_COMPOSED = [  # name, output format, size, extras, least PSNR
    ("j420.jpg", "RGB24", None, "", 45), ("j420.jpg", "BGRA32", None, "", 45), ("j420.jpg", "RGB24", (320, 240), "", 30),
    ("j1080.jpg", "RGB24", None, "", 42), ("j800.jpg", "RGB24", None, "", 45), ("j800.jpg", "BGRA32", None, "", 45),
    ("prgb.png", "BGRA32", None, "", 70), ("prgb.png", "RGB24", None, "", 70),
    ("b24.bmp", "BGR24", None, "", 70), ("t24.tga", "RGB24", None, "ENCODING TGA", 70),
]
STILL_REFUSED = [("jprog.jpg", ""), ("jcmyk.jpg", ""), ("jsplit.jpg", ""), ("pgrey.png", ""), ("pgreya.png", ""), ("p16.png", ""),
                 ("pinter.png", ""), ("podd.png", ""), ("pkey.png", ""), ("gtrans.gif", ""), ("trle.tga", "ENCODING TGA")]
STILL_CUT = [("jcut.jpg", ""), ("pcut.png", ""), ("gcut.gif", ""), ("bcut.bmp", ""), ("tcut.tga", "ENCODING TGA"), ("p6cut.ppm", "")]
STILL_SMOKE = ("j420.jpg", "prgb.png")


def native_params():
    """The native stills, two of them in the smoke tier, the rest slow."""
    return [pytest.param(name, extra, least, alpha, id=name,
                         marks=pytest.mark.smoke if name in STILL_SMOKE else pytest.mark.slow)
            for name, extra, least, alpha in STILL_NATIVE]


@pytest.mark.parametrize("name,extra,least,with_alpha", native_params())
def test_still_native(device, material, record_property, name, extra, least, with_alpha):
    """Every format to RGBA at its own size, straight from the decoder: lossless ones exact, JPEG and GIF by PSNR."""
    code, us, r, raw = still_decode(device, material, name, "RGBA32", extra=extra)
    assert code == 0, f"VC_DecodeImage {code}: {r.kv.get('fail')}"
    ref = still_reference(material, name)
    h, w = ref.shape[:2]
    rgb, alpha = still_rgb("RGBA32", raw, w, h)
    score = psnr(rgb, ref[:, :, :3])
    record_property("psnr_db", round(score, 1))
    record_property("us", us)
    assert score >= least, f"PSNR {score:.1f} dB, below {least}"
    if with_alpha:
        assert np.array_equal(alpha, ref[:, :, 3]), "the alpha differs"


@pytest.mark.slow
@pytest.mark.parametrize("name,fmt,size,extra,least", [pytest.param(*p, id=f"{p[0]}-{p[1]}{'-%dx%d' % p[2] if p[2] else ''}")
                                                        for p in STILL_COMPOSED])
def test_still_composed(device, material, record_property, name, fmt, size, extra, least):
    """Formats and sizes the decoder does not write: the converter after it (VC_DecodeImage composes them)."""
    code, us, r, raw = still_decode(device, material, name, fmt, size, extra)
    assert code == 0, f"VC_DecodeImage {code}: {r.kv.get('fail')}"
    ref = still_reference(material, name)[:, :, :3]
    if size:  # the reference box-filtered down
        fy, fx = ref.shape[0] // size[1], ref.shape[1] // size[0]
        ref = ref.reshape(size[1], fy, size[0], fx, 3).mean(axis=(1, 3))
    h, w = ref.shape[:2]
    rgb, _ = still_rgb(fmt, raw, w, h)
    score = psnr(rgb, ref)
    record_property("psnr_db", round(score, 1))
    record_property("us", us)
    assert score >= least, f"PSNR {score:.1f} dB, below {least}"


@pytest.mark.slow
@pytest.mark.parametrize("name,extra", [pytest.param(*p, id=p[0]) for p in STILL_REFUSED])
def test_still_refused(device, material, name, extra):
    """What the decoder gets wrong or dies on is refused from the header at once (-6)."""
    code, us, r, _ = still_decode(device, material, name, "RGBA32", extra=extra, fetch=False)
    assert code == -6 and us <= 1_000_000, f"VC_DecodeImage {code} in {us // 1000} ms"


@pytest.mark.slow
@pytest.mark.parametrize("name,extra", [pytest.param(*p, id=p[0]) for p in STILL_CUT])
def test_still_cut_short(device, material, name, extra):
    """A picture cut at 70 percent: refused as incomplete (-8), at once."""
    code, us, r, _ = still_decode(device, material, name, "RGBA32", extra=extra, fetch=False)
    assert code == -8 and us <= 1_000_000, f"VC_DecodeImage {code} in {us // 1000} ms"


@pytest.mark.slow
@pytest.mark.parametrize("name,fmt,want", [("jhuge.jpg", "I420", 0), ("j6016.jpg", "RGBA32", 0), ("j4000.jpg", "RGB24", 0),
                                           ("j6000.jpg", "RGBA32", 0), ("jhuge.jpg", "RGBA32", 0), ("j420.jpg", "RGBA32", 0)],
                         ids=["8000x6000-i420", "6016x4000-rgba", "4000x3000-rgb24-composed", "6000x4000-rgba-composed",
                              "8000x6000-rgba", "small-after"])
def test_still_big(device, material, record_property, name, fmt, want):
    """Big pictures straight and through the converter, 8000x6000 to RGBA (192 MB) past what a bulk carries; a small one decodes after."""
    code, us, r, _ = still_decode(device, material, name, fmt, fetch=False)
    record_property("ms", us // 1000)
    assert code == want, f"VC_DecodeImage {code}, wanted {want}"
    if code == 0:
        assert r.has("image.written"), "nothing written"


def test_still_1080p_time(device, material, baselines):
    """A 1080p JPEG to RGB24 by the image call: its time (best of three), against the baseline and under 300 ms."""
    us = still_time(device, material, "j1080.jpg", "RGB24")
    assert us < 300_000, f"{us // 1000} ms"
    baselines.check("still_1080p_rgb24.image.us", us, higher=False, tolerance="25%")


HEADERS = {  # name: codec, width, height, transparency, frames, as VC_GetImageInfo() must report them
    "prgb.png": ("PNG", 640, 480, "none", 1), "ppal.png": ("PNG", 640, 480, "mask", 1), "palpha.png": ("PNG", 640, 480, "alpha", 1),
    "pgreya.png": ("PNG", 640, 480, "alpha", 1), "pinter.png": ("PNG", 640, 480, "none", 1), "jprog.jpg": ("JPEG", 640, 480, "none", 1),
    "jcmyk.jpg": ("JPEG", 640, 480, "none", 1), "trle.tga": ("TGA", 640, 480, "none", 1), "t32.tga": ("TGA", 640, 480, "alpha", 1),
    "ganim.gif": ("GIF", 640, 480, "none", 2), "gtrans.gif": ("GIF", 640, 480, "mask", 1), "b32.bmp": ("BMP", 640, 480, "alpha", 1),
    "btop.bmp": ("BMP", 640, 480, "none", 1), "p5.pgm": ("PPM", 640, 480, "none", 1),
}


@pytest.mark.slow
@pytest.mark.parametrize("name", list(HEADERS), ids=list(HEADERS))
def test_image_header(device, material, name):
    """VC_GetImageInfo(): codec, size, transparency and frames as the header says."""
    extra = "ENCODING TGA" if name.endswith(".tga") else ""
    code, us, r, _ = still_decode(device, material, name, "RGBA32", extra=extra, fetch=False)
    got = (r.kv.get("header.codec"), int(r.num("header.width")), int(r.num("header.height")), r.kv.get("header.transparency"),
           int(r.num("header.frames")))
    assert got == HEADERS[name], f"header {got}, wanted {HEADERS[name]}"


def test_example_viewer(device, material):
    """VCShow, the picture example: the bars in a window until Ctrl-C; a progressive JPEG falls back at once (return code 5)."""
    picture = material.ram("pattern.jpg")
    job = device.start("VCShow", picture)
    try:
        time.sleep(3)
        x, y, w, h = device.window(picture)  # VCShow titles its window with the file's name
        problems = judge_screen_bars(device, picture_bars(x + 4, y + 11, w - 8, h - 15), "vcshow")
    finally:
        job.stop()
        text = job.wait(10)
    expect(text, r"pattern\.jpg: 1920x1080, in a \d+x\d+ window", "VCShow's line")
    assert not problems, "; ".join(problems)
    rc, prog = device.run_text("VCShow", material.ram("prog.jpg"), timeout=30)
    assert rc == 5, f"return code {rc}"
    expect(prog, r"does not decode this picture", "VCShow's refusal")


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left, no stray message."""
    assert_nothing_left(device)
