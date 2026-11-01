# SPDX-License-Identifier: BSD-3-Clause
"""vcjpeg.datatype as the installer installs it: routing, pixels, the hand-over, the idle close.

The class and descriptor are in SYS: (the install step replaced them with the build's), the descriptor added from
S:User-Startup at boot.
"""
import re
import time

import pytest

from harness.pictures import psnr
from harness.results import expect
from harness.tools import MIB, assert_clean, assert_nothing_left, codec_memory, dt_load, dt_page, status, still_reference, still_rgb

ROUTES = [("j1080.jpg", True), ("j4000.jpg", True), ("jodd.jpg", True), ("j420.jpg", False), ("jprog.jpg", False), ("jcmyk.jpg", False)]
PAGES = [pytest.param(("j1080.jpg", "jodd.jpg"), 10, id="two-sizes"),  # side by side, these ran the codec heap out
         pytest.param(("j4000.jpg", "j1080.jpg", "jodd.jpg", "j420.jpg"), 5, id="four-sizes"),  # side by side, this stopped the firmware; the small one is the system's
         pytest.param(("j1080.jpg", "jodd.jpg"), 24, id="past-the-timeout")]  # a queue longer than one call's timeout, small pictures to keep it in RAM
PAGE_HEAP_LEFT = 32 * MIB  # the least left, a picture at a time: side by side the pages left 1 or 2 MiB
PAST_THE_RAM = ("j6000.jpg", "j4000.jpg", "j1080.jpg", "j420.jpg")  # five of each take more than the RAM the VPU reaches at gpu_mem=192
PAGES_RUNNING = 5


def test_descriptor_takes_large_jpegs(device):
    """VCInfo says the descriptor takes the large JPEGs."""
    rc, text = device.run_text("VCInfo", "")
    expect(text, r"^  JPEG datatype:  vcjpeg\.datatype takes the large JPEGs", "VCInfo's summary")


@pytest.mark.parametrize("name,ours", [pytest.param(n, o, id=n, marks=pytest.mark.smoke if n == "j1080.jpg" else []) for n, o in ROUTES])
def test_routing(device, material, record_property, name, ours):
    """Which class each JPEG gets: baseline ones of 1.8 MP and more ours, small, progressive and CMYK ones the system's; all load."""
    desc, code, us, _, _ = dt_load(device, material, name)
    record_property("loaded", f"{desc}, {code} in {us // 1000} ms")
    assert code == 0, f"NewDTObject {code}"
    assert (desc == "VCJPEG") == ours, f"descriptor {desc}"


@pytest.mark.parametrize("name", ["j1080.jpg", "jodd.jpg", "j420.jpg"])
def test_pixels(device, material, baselines, name):
    """The pixels against Pillow: 1080p straight into picture.datatype's buffer, 1800x1101 through the class's own, 640x480 the system's."""
    desc, code, us, _, raw = dt_load(device, material, name, out=True)
    assert code == 0 and raw is not None, f"{desc}: {code}"
    ref = still_reference(material, name)[:, :, :3]
    h, w = ref.shape[:2]
    rgb, _ = still_rgb("RGB24", raw, w, h)
    baselines.check(f"datatype_{name}.psnr_db", round(psnr(rgb, ref), 1), higher=True, tolerance=2.0)
    for _ in range(2):  # the time: best of three loads
        us = min(us, dt_load(device, material, name)[2])
    baselines.check(f"datatype_{name}.us", us, higher=False, tolerance="25%")


@pytest.mark.parametrize("name,handed", [("j6000.jpg", False), ("jhuge.jpg", False), ("jsplit.jpg", True)])
def test_hand_over(device, material, driver_log, name, handed):
    """6000x4000 and 8000x6000 on the VPU twice, the same pixels; a header too long without its metadata too refused (-6) and handed to the system's class both times."""
    mark = driver_log.mark()
    first = dt_load(device, material, name)
    second = dt_load(device, material, name)
    for desc, code, _, total, _ in (first, second):
        assert desc == "VCJPEG" and code == 0 and total is not None, f"{desc}, {code}"
    assert first[3] == second[3], f"the pixels differ between the two loads ({first[3]} and {second[3]})"
    said = driver_log.since(mark).find(rf"\[vcjpeg\] \S*{re.escape(name)}: the VPU said (-?\d+), to software")
    assert said == (["-6", "-6"] if handed else []), f"handed over with {said}"


def test_idle_close(device, material, driver_log, record_property):
    """Some seconds after its last picture the class closes the library and the codec memory is back; the next picture reopens it."""
    mark = driver_log.mark()
    first = dt_load(device, material, "j6000.jpg")
    time.sleep(12)  # VCJPEG_IDLE_MS (5 s) to twice it, and the close
    free, total = codec_memory(status(device))
    record_property("codec memory", f"{free >> 20} of {total >> 20} MiB free 12 s after the last picture")
    assert free >= total - 6 * MIB, "the codec memory is not back"
    again = dt_load(device, material, "j6000.jpg")
    assert again[0] == "VCJPEG" and again[1] == 0 and again[3] == first[3]
    log = driver_log.since(mark)
    log.expect(r"\[vcjpeg\] idle: videocodec\.library closed", "the class's log")
    log.forbid(r"\[vcjpeg\] \S*j6000\.jpg: the VPU said", "the class's log")


def test_a_page_past_the_ram(device, material, record_property):
    """Five each of 24, 12 and 2 MP and small JPEGs at once, page after page, their buffers filling the RAM the VPU reaches:
    what it cannot hold goes to software, all load, and the VPU decodes as before.  A frame refused at the format change,
    the pipeline holding part of a picture, left MMAL dead by the third page.
    """
    alone = dt_load(device, material, "j1080.jpg")
    assert alone[0] == "VCJPEG" and alone[1] == 0, f"a lone load: {alone}"
    pages = []
    for page in range(PAGES_RUNNING):
        r = dt_page(device, material, PAST_THE_RAM, 5)
        assert r.passed, f"page {page + 1}: {r.kv.get('fail')}"
        pictures = r.group("picture")
        assert len(pictures) == 20 and all(p["rc"] == "0" for p in pictures), f"page {page + 1}: {pictures}"
        pages.append(r.num("page.us") // 1000)
        device.wait_agent(180)
        again = dt_load(device, material, "j1080.jpg")
        assert (again[0], again[3]) == (alone[0], alone[3]), f"after page {page + 1} of {PAGES_RUNNING}, in {pages} ms: {again}"
    record_property("pages", f"{pages} ms")
    time.sleep(12)  # the idle close (test_idle_close)
    after = status(device)
    assert_clean(after)
    free, total = codec_memory(after)
    assert free >= total - 6 * MIB, f"{free >> 20} of {total >> 20} MiB free 12 s after the pages"


@pytest.mark.parametrize("names,copies", PAGES)
def test_many_at_once(device, material, record_property, names, copies):
    """Many JPEGs at once, a process each (a browser page): all in flight together, each with the pixels it has alone.

    The image calls queue, so none times out, none runs out of memory and the codec heap never holds more than a
    picture's worth.  The sums say none went to software (its decode differs) and none got another's pixels.  The VPU
    then decodes as before and gives everything back.
    """
    alone = {name: dt_load(device, material, name) for name in names}
    assert all(load[1] == 0 for load in alone.values()), f"a lone load failed: {alone}"
    r = dt_page(device, material, names, copies)
    assert r.passed, f"the page: {r.kv.get('fail')}"
    pictures = r.group("picture")
    assert len(pictures) == len(names) * copies

    record_property("page", f"{len(pictures)} pictures in {r.num('page.us') // 1000} ms, "
                            f"{r.num('page.mem_lowest') >> 20} MiB codec memory left at the lowest")
    ends = [int(p["start_us"]) + int(p["us"]) for p in pictures]
    assert max(int(p["start_us"]) for p in pictures) < min(ends), "the first picture ended before the last began: not at once"
    for p in pictures:
        name = p["file"].rpartition("/")[2]
        assert (p["descriptor"], p["sum"]) == (alone[name][0], alone[name][3]), f"{p}: alone {alone[name][0]}, sum {alone[name][3]}"
    assert r.num("page.mem_lowest") >= PAGE_HEAP_LEFT, f"{r.num('page.mem_lowest') >> 20} MiB of the codec heap left at the lowest"

    device.wait_agent(180)  # the PI4's agent can stall a minute or two with this much memory moved
    again = dt_load(device, material, names[0])
    assert again[1] == 0 and again[3] == alone[names[0]][3], "the VPU decodes otherwise after the page"
    time.sleep(12)  # the idle close (test_idle_close)
    after = status(device)
    assert_clean(after)
    free, total = codec_memory(after)
    assert free >= total - 6 * MIB, f"{free >> 20} of {total >> 20} MiB free 12 s after the page"


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left after the pictures."""
    assert_nothing_left(device)
