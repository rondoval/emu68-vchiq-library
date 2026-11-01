# SPDX-License-Identifier: BSD-3-Clause
"""VPUPlay, the display path: full screen, depths, a window, two players, the shape change on screen; VCPlay the example.

A running rig's output file cannot be read (the shell's redirection holds it exclusively), so a screen test learns the
display geometry from a short foreground run first, then judges the looping background run by the screen alone.
"""
import time

import pytest

from harness.results import expect
from harness.tools import assert_nothing_left, fitted, judge_screen_bars, picture_bars, vpuplay

BORDERS = (4, 11, 4, 2)  # a Workbench window's left, top, right and bottom borders when nothing better is known
SETTLE_S = 3  # from a player's start to its screen showing the bars


def geometry(device, args):
    """A two-frame foreground run of VPUPlay with @args: the geometry the real run will have."""
    r = vpuplay(device, f"{args} FRAMES 2", 60)
    assert r.passed, f"the geometry run: {r.kv.get('fail')}"
    return r


def screen_bars(r):
    """The pattern's bars on a full screen of r's size, the picture fitted in with its aspect kept."""
    x, y, w, h = fitted(int(r.num("screen.width")), int(r.num("screen.height")))
    return picture_bars(x, y, w, h)


def window_bars(outer, borders):
    """The bars inside a VPUPlay window of @outer (x, y, w, h): the picture fitted into the inner area as the player fits it."""
    x, y, w, h = outer
    left, top, right, bottom = borders
    inner_w, inner_h = w - left - right, h - top - bottom
    fit_w = min(inner_w, inner_h * 16 // 9) & ~31
    fit_h = (fit_w * 9 // 16) & ~1
    return picture_bars(x + left + (inner_w - fit_w) // 2, y + top + (inner_h - fit_h) // 2, fit_w, fit_h)


@pytest.mark.smoke
def test_full_screen(device, material, baselines, record_property):
    """Full screen at the Workbench's depth: three parts, every frame of the clip at its rate."""
    r = vpuplay(device, material.ram("clip42.h264"), 90)
    assert r.passed, r.kv.get("fail")
    assert r.num("play.shown") == 300 and r.num("play.parts") == 3
    record_property("late", r.kv.get("play.late"))
    baselines.check("fullscreen.play.fps", r.num("play.fps"), higher=True, tolerance="10%")


def test_full_screen_zero_copy(device, material, driver_log):
    """Full screen: the ISP writes the screen's bitmap in VRAM itself (its output port zero copy)."""
    mark = driver_log.mark()
    r = vpuplay(device, material.ram("clip42.h264"), 90)
    assert r.passed, r.kv.get("fail")
    driver_log.since(mark).expect(r"ril\.isp port \d+: zero copy on", "the codec's log")


@pytest.mark.parametrize("depth", [32, 24])
def test_depths(device, material, depth):
    """Full screen at 32 and 24 bits with the colour bars: three parts, and every bar the colour it should be."""
    args = f"{material.ram('pattern.h264')} DEPTH {depth}"
    geo = geometry(device, args)
    assert geo.num("screen.parts") == 3, f"{geo.kv.get('screen.parts')} picture heights"
    assert geo.num("screen.depth") == depth
    job = device.start("VPUPlay", f"{args} LOOP")
    try:
        time.sleep(SETTLE_S)
        problems = judge_screen_bars(device, screen_bars(geo), f"depth{depth}", tolerance=12 if depth == 16 else 8)
    finally:
        job.stop()
        r = job.wait(10)
    assert r.passed, r.kv.get("fail")
    assert not problems, "; ".join(problems)


def test_window_moves_and_covers(device, material):
    """In a window: moved, grown and shrunk (WINDOWTEST), covered and uncovered, the bars right each time."""
    args = f"{material.ram('pattern.h264')} WINDOW"
    geo = geometry(device, args)
    inner = (int(geo.num("window.inner_width")), int(geo.num("window.inner_height")))
    job = device.start("VPUPlay", f"{args} WINDOWTEST LOOP")
    problems = []
    try:
        time.sleep(SETTLE_S)
        x, y, w, h = device.window("VPUPlay")
        borders = (BORDERS[0], h - inner[1] - BORDERS[3], w - inner[0] - BORDERS[0], BORDERS[3])  # the real top and right
        problems += judge_screen_bars(device, window_bars((x, y, w, h), borders), "start")
        time.sleep(16)  # the three steps, at 6, 12 and 18 s of play
        outer = device.window("VPUPlay")
        problems += judge_screen_bars(device, window_bars(outer, borders), "resized")
        x, y, w, h = outer
        device.exec(f'Run >NIL: NewShell "CON:{x + 40}/{y + 40}/{w // 2}/{h // 2}/TESTCOVER/CLOSE"', timeout=15)
        time.sleep(2)
        device.screenshot((x, y, w, h), name="covered")
        cx, cy, cw, ch = device.window("TESTCOVER")
        device.click(cx + 8, cy + 5)  # the console's close gadget (a key would stop the player)
        time.sleep(2)
        problems += judge_screen_bars(device, window_bars(outer, borders), "uncovered")
    finally:
        job.stop()
        r = job.wait(10)
    assert r.passed, r.kv.get("fail")
    assert not r.has("output.error"), "a resize was refused"
    assert len(r.group("output")) >= 3, "the resizes did not reach the output"
    moves, resizes, refreshes = r.num("window.moves"), r.num("window.resizes"), r.num("window.refreshes")
    assert moves >= 1 and resizes >= 2 and refreshes >= 1, f"{moves} moves, {resizes} resizes, {refreshes} refreshes"
    assert not problems, "; ".join(problems)


def test_loop(device, material):
    """LOOP: the file starts over at its end without a seam, until Ctrl-C."""
    job = device.start("VPUPlay", f"{material.ram('clip42.h264')} LOOP")
    time.sleep(25)
    job.stop()
    r = job.wait(10)
    assert r.passed, r.kv.get("fail")
    assert r.num("play.loops") >= 1, "never started over"


def test_no_room_for_a_decoder(device, material):
    """Two decoders of 16 reference frames leave no room for a third: refused at its first packet, not a wait for pictures; the two play on."""
    stream = material.ram("refs16.h264")
    jobs = [device.start("VPUPlay", f"{stream} WINDOW LOOP") for _ in range(2)]
    try:
        time.sleep(6)
        started = time.time()
        r = vpuplay(device, f"{stream} WINDOW FRAMES 30", 30)
        took = time.time() - started
        assert r.rc == 20 and r.kv.get("fail") == "VC_SendPacket failed", f"rc {r.rc}, {r.kv.get('fail')}"
        assert took < 4, f"refused after {took:.1f} s"
    finally:
        for job in jobs:
            job.stop()
    for job in jobs:
        result = job.wait(10)
        assert result.passed, result.kv.get("fail")


def test_two_players(device, material):
    """Two windows at once, the second started while the first plays."""
    a = device.start("VPUPlay", f"{material.ram('clip42.h264')} WINDOW FRAMES 450")
    time.sleep(3)
    b = device.start("VPUPlay", f"{material.ram('clip42.h264')} WINDOW FRAMES 450")
    ra, rb = a.wait(90), b.wait(90)
    for r in (ra, rb):
        assert r.passed, r.kv.get("fail")
        assert not r.has("play.idle")


def test_empty_stream(device):
    """NIL: as the stream, its end mark the only packet: it ends at once, not at the idle timeout."""
    r = vpuplay(device, "NIL:", 60)
    assert r.flag("play.no_picture") and r.rc == 5
    assert r.num("play.total_us") <= 3_000_000, f"ended after {r.num('play.total_us') / 1e6:.1f} s"


@pytest.mark.slow
def test_shape_change_on_screen(device, material):
    """LOOP over the bars at 1080p then 720p: the picture shrinks and grows inside the running tunnel; the bars stay right."""
    args = material.ram("two-pattern.h264")
    bars = screen_bars(geometry(device, args))
    job = device.start("VPUPlay", f"{args} LOOP")
    problems = []
    try:
        t0 = time.time()
        for name, at in (("1080p", SETTLE_S), ("720p", SETTLE_S + 2.5), ("1080p-again", SETTLE_S + 4.5)):
            time.sleep(max(0.0, t0 + at - time.time()))
            problems += [f"{name}: {p}" for p in judge_screen_bars(device, bars, name)]
    finally:
        job.stop()
        r = job.wait(10)
    assert r.passed, r.kv.get("fail")
    assert r.num("play.shown") >= 200, f"{r.kv.get('play.shown')} frames shown in the time"
    assert len(r.group("stream")) == 1, "a shape change reported from inside the tunnel (the ISP follows it in the hardware)"
    assert not problems, "; ".join(problems)


def test_pixel_aspect_on_screen(device, material, driver_log):
    """A 720x576 stream of 64:45 pixels placed 16:9 on screen, as a TV shows it, not 5:4."""
    mark = driver_log.mark()
    r = vpuplay(device, material.ram("wide.h264"), 60)
    assert r.passed, r.kv.get("fail")
    places = driver_log.since(mark).find(r"ISP: format \d+, (\d+)x(\d+) at")
    assert places, "no ISP line in the debug log"
    w, h = (int(n) for n in places[-1])
    assert abs(w * 9 - h * 16) * 50 <= h * 16, f"placed {w}x{h}, not 16:9"


@pytest.mark.smoke
def test_example_player(device, material):
    """VCPlay, the minimal player: the whole clip, return code 0."""
    rc, text = device.run_text("VCPlay", material.ram("clip42.h264"), timeout=45)
    assert rc == 0, f"return code {rc}:\n{text[-600:]}"
    expect(text, r"300 frames shown", "VCPlay's summary")


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left, no stray message."""
    assert_nothing_left(device)
