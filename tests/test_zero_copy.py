# SPDX-License-Identifier: BSD-3-Clause
"""Zero copy: pictures imported into the VPU (SMEM) instead of copied, read in the codec's debug log."""
import pytest

from harness.pictures import psnr
from harness.tools import (ZC_TROUBLE, assert_nothing_left, codec_memory, status, still_decode, still_reference, still_rgb,
                           still_time, vctest)


def no_trouble(log):
    """None of the import trouble lines in a log slice."""
    for t in ZC_TROUBLE:
        log.forbid(r"\[videocore\] .*" + t, "the codec's log")


@pytest.fixture(scope="module")
def module_mark(device, capture):
    """Where the log was when the module started: the final check reads from here."""
    device.ram_clear()  # the suite's material sits in the RAM these big pictures need
    device.flush()  # a class left loaded (the datatype's) keeps its components, which then log nothing
    return capture.mark()


@pytest.mark.smoke
def test_reach_and_imports(device, material, driver_log, module_mark, record_property):
    """The VPU's reach in STATUS; a 6000x4000 JPEG to RGB24 through the converter with every port imported; the memory back after."""
    r = status(device)
    assert r.has("vchiq.reach.start") and r.num("vchiq.reach.end") > r.num("vchiq.reach.start"), "no reach in STATUS"
    record_property("reach", f"{r.kv['vchiq.reach.start']}-{r.kv['vchiq.reach.end']} at bus {r.kv['vchiq.reach.bus']}")
    mark = driver_log.mark()
    code, us, _, _ = still_decode(device, material, "j6000.jpg", "RGB24", fetch=False)
    assert code == 0, f"VC_DecodeImage {code}"
    record_property("6000x4000 to RGB24", f"{us // 1000} ms")
    free, total = codec_memory(status(device))
    record_property("codec memory after", f"{free >> 20} of {total >> 20} MiB free")
    log = driver_log.since(mark)
    log.expect(r"aggregator\.pipeline:ril\.image_decode:video_convert port \d+: zero copy on", "the decoder's output")
    log.expect(r"ril\.isp port 1: zero copy on", "the ISP's input")
    log.expect(r"ril\.isp port 2: zero copy on", "the ISP's output")
    no_trouble(log)


@pytest.mark.parametrize("misalign", [True, False], ids=["misaligned", "aligned"])
def test_misaligned_goes_through_own_frame(device, material, driver_log, module_mark, baselines, misalign):
    """A picture 4 bytes past a cache line: the ISP writes the backend's own frame, one copy; on a line the caller's; imports both ways, same pixels."""
    mark = driver_log.mark()
    code, us, _, raw = still_decode(device, material, "j1080.jpg", "RGB24", misalign=misalign)
    assert code == 0 and raw is not None, f"VC_DecodeImage {code}"
    ref = still_reference(material, "j1080.jpg")[:, :, :3]
    h, w = ref.shape[:2]
    rgb, _ = still_rgb("RGB24", raw, w, h)
    which = "misaligned" if misalign else "aligned"
    baselines.check(f"zero_copy_{which}.psnr_db", round(psnr(rgb, ref), 1), higher=True, tolerance=2.0)
    us = min(us, still_time(device, material, "j1080.jpg", "RGB24", misalign=misalign, times=2))
    baselines.check(f"zero_copy_{which}.image.us", us, higher=False, tolerance="25%")
    log = driver_log.since(mark)
    log.expect(r"ril\.isp port 1: zero copy on", "the ISP's input")
    log.expect(r"ril\.isp port 2: zero copy on", "the ISP's output")
    no_trouble(log)


def test_decoder_input_imported(device, material, driver_log, module_mark):
    """Input packets of 1 MiB go by import: three hundred frames bit exact and a JPEG; the 256 KiB ones of FRAMED stay on bulk."""
    framed = f"DECODE {material.ram('clip42.h264')} REF {material.ram('clip42.framemd5')} FRAMED"
    mark = driver_log.mark()
    r = vctest(device, f"{framed} PACKET 1048576", 180)
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0
    log = driver_log.since(mark)
    log.expect(r"ril\.video_decode port 1: zero copy on", "H.264 input")
    no_trouble(log)

    mark = driver_log.mark()
    r = vctest(device, f"DECODE {material.ram('pic.jpg')} PACKET 1048576", 60)
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.frames") == 1 and r.num("decode.width") == 1920
    log = driver_log.since(mark)
    log.expect(r"ril\.video_decode port 1: zero copy on", "MJPEG input")
    no_trouble(log)

    mark = driver_log.mark()
    r = vctest(device, framed, 180)
    assert r.passed, r.kv.get("fail")
    driver_log.since(mark).forbid(r"ril\.video_decode port 1: zero copy on", "packets of 256 KiB")


@pytest.mark.smoke
def test_nothing_left(device, capture, module_mark):
    """Nothing left: no session, no stray message; and no import trouble in the log since the module began."""
    assert_nothing_left(device)
    if capture.ok:
        no_trouble(capture.since(module_mark))
