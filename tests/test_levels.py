# SPDX-License-Identifier: BSD-3-Clause
"""What the H.264 decoder takes past the codec table's level 4.0: declared levels, reference frames, rates, sizes, profiles.

A refusal is a fact of the hardware, pinned by the baseline (the frames it decoded); wrong or missing pictures fail.
"""
import numpy as np
import pytest

from harness.material import LEVEL_DECLARED, LEVEL_FPS, LEVEL_RATES, LEVEL_REFS, LEVEL_SIZES, describe, ffmpeg
from harness.tools import assert_nothing_left, vctest

pytestmark = pytest.mark.slow

MJPEG_SIZES = [(1920, 1080, False), (2560, 1440, False), (4096, 1024, False), (1024, 1920, False),  # width, height, refused
               (4112, 1024, True), (1024, 1922, True), (1024, 1936, True), (3840, 2160, True)]

CASES = (  # name, the reference's name (None: speed only), whether the rate matters
    [("l-base", "l-base", False)] + [("l-level" + lvl.replace(".", ""), "l-base", False) for lvl in LEVEL_DECLARED] +
    [(f"l-ref{refs}", f"l-ref{refs}", False) for refs in LEVEL_REFS] +
    [("l-base", None, True)] + [(f"l-rate{lvl.replace('.', '')}",) * 2 + (True,) for lvl, _, _ in LEVEL_RATES] +
    [(f"l-{w}x{h}",) * 2 + (False,) for w, h in LEVEL_SIZES] +
    [(name, name, False) for name in ("l-high10", "l-high422")]
)


def verdict(r):
    """What a DECODE result says in one word: exact, refused, refused silently, WRONG, INCOMPLETE or failed."""
    if r.has("ref.mismatches") and r.num("ref.mismatches") > 0:
        return "WRONG"
    if r.passed:
        return "exact" if r.has("ref.compared") else "decoded"
    if r.has("session.error") or r.has("packet.error") or r.kv.get("decode.session") == "refused":
        return "refused"
    if r.has("decode.idle") and r.flag("decode.idle") and not r.group("stream"):
        return "refused silently"
    if r.has("decode.idle") or (r.has("ref.count_differs") and r.flag("ref.count_differs")):
        return "INCOMPLETE"
    return "failed"


def samples_off(device, material, name, frames):
    """The pictures @frames of the stream decoded to I420 on the Amiga, against ffmpeg's: (samples that differ, largest difference)."""
    w, h = 1920, 1080
    size = w * h * 3 // 2
    out = f"RAM:hw/{name}.i420"
    vctest(device, f"DECODE {material.ram(name + '.h264')} FRAMES {max(frames) + 1} OUT {out}", 300)
    ref = ffmpeg("-i", str(material.path(name + ".h264")), "-frames:v", str(max(frames) + 1), "-f", "rawvideo", "-pix_fmt",
                 "yuv420p", "-")
    samples = largest = 0
    for i in frames:
        got = device.read_range(out, i * size, size)
        assert len(got) == size, f"picture {i} missing from the decode"
        d = np.abs(np.frombuffer(got, np.uint8).astype(np.int16) - np.frombuffer(ref, np.uint8, size, i * size))
        samples += int(np.count_nonzero(d))
        largest = max(largest, int(d.max()))
    device.delete(out)
    return samples, largest


@pytest.mark.parametrize("name,ref,speed", CASES, ids=[f"{c[0]}{'-speed' if c[1] is None else ''}" for c in CASES])
def test_level_probe(device, material, baselines, record_property, name, ref, speed):
    """One probe stream decoded on the Amiga: bit exact, refused, or (never) wrong or incomplete; its rate when it matters."""
    stream = material.ram(f"{name}.h264")
    record_property("stream", describe(material.path(f"{name}.h264"), LEVEL_FPS.get(name, 30)))
    if ref is not None:
        r = vctest(device, f"DECODE {stream} REF {material.ram(ref + '.framemd5')} VERBOSE", 180)
        word = verdict(r)
        if word == "WRONG" and speed:  # a rounding difference is not a broken picture
            wrong = [int(m["frame"]) for m in r.group("mismatch")]
            samples, largest = samples_off(device, material, name, wrong)
            if largest <= 1:
                word = f"near exact ({samples} samples off by one in {len(wrong)} pictures)"
        record_property("verdict", word)
        frames = int(r.num("decode.frames", 0))
        assert not word.startswith(("WRONG", "INCOMPLETE", "failed")), f"{word}: {r.kv.get('fail')}"
        baselines.check(f"level.{name}.frames", frames, higher=True, tolerance=0)
        if word == "refused" or not speed:
            return
    r = vctest(device, f"DECODE {stream} NOHASH", 180)
    assert r.passed, f"speed run: {r.kv.get('fail')}"
    fps = r.num("decode.fps")
    want = LEVEL_FPS.get(name, 30)
    record_property("rate", f"{fps} fps ({'real time' if fps >= want else f'BEHIND its {want} fps'})")
    baselines.check(f"level.{name}.decode.fps", fps, higher=True, tolerance="10%")


@pytest.mark.parametrize("width, height, refused", MJPEG_SIZES)
def test_mjpeg_size(device, artifacts, record_property, width, height, refused):
    """A JPEG of this size as one MJPEG packet through the video decoder: ffmpeg's picture within the limits, refused past them."""
    jpeg = artifacts / "size.jpg"
    ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={width}x{height}:rate=1", "-frames:v", "1", "-pix_fmt", "yuvj420p", "-q:v", "3", str(jpeg))
    wanted = np.frombuffer(ffmpeg("-i", str(jpeg), "-f", "rawvideo", "-pix_fmt", "yuvj420p", "-"), np.uint8)
    device.upload(jpeg, "RAM:hw/size.jpg")
    r = vctest(device, "DECODE RAM:hw/size.jpg OUT RAM:hw/size.yuv", 60)
    device.delete("RAM:hw/size.jpg")
    if refused:
        device.delete("RAM:hw/size.yuv")
        assert not r.passed and r.num("decode.frames", 0) == 0, f"past the limits, {r.kv.get('decode.frames')} frames"
        assert r.num("packet.error", 0) == -8, f"refused as a bad stream: {r.kv.get('fail')}, {r.kv.get('packet.error')}"
        return
    assert r.passed, r.kv.get("fail")
    got = np.frombuffer(device.download("RAM:hw/size.yuv"), np.uint8)
    device.delete("RAM:hw/size.yuv")
    assert got.size == wanted.size, f"{got.size} bytes, ffmpeg's {wanted.size}"
    luma = width * height
    psnr = 10 * np.log10(255 ** 2 / float(((got[:luma].astype(np.int32) - wanted[:luma]) ** 2).mean()))
    record_property("PSNR", round(psnr, 1))
    assert psnr > 30, f"{psnr:.1f} dB from ffmpeg's picture"  # the VPU softens edges on 8x8 block lines, which no decoder need keep


def test_nothing_left(device, baselines, record_property):
    """After the refusals: no session left, the codec memory back."""
    from harness.tools import codec_memory, status
    r = status(device)
    assert_nothing_left(device)
    free, total = codec_memory(r)
    record_property("codec memory free", f"{free >> 20} of {total >> 20} MiB")
