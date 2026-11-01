# SPDX-License-Identifier: BSD-3-Clause
"""videocore.audio, the AHI driver, through ahi.device's low-level API (VCTest AHITONE, AHIWALK): its three modes."""
import time

import pytest

from harness.tools import assert_clean, status, vctest

MODE_16BIT, MODE_HIFI, MODE_71 = 0x0E680001, 0x0E680002, 0x0E680003  # DEVS:AudioModes/VIDEOCORE; the default output, HDMI0
TROUBLE = r"\[videocore\.audio\] .*(goes on without the VPU|did not open|never came back)"


@pytest.fixture(scope="module")
def services_before(device):
    """The services open before the plays: every AUDS service they open must be closed again."""
    return int(status(device).num("vchiq.services"))


def tone(device, mode, ms=4000, extra=""):
    """VCTest AHITONE through @mode for @ms, which must pass and play about that long."""
    r = vctest(device, f"AHITONE MODE {mode} MS {ms} {extra}", ms // 1000 + 30)
    assert r.passed, r.kv.get("fail")
    played = r.num("ahi.us") / 1000
    assert ms - 200 <= played <= ms + 1500, f"a {ms} ms tone played {played:.0f} ms"
    return r


@pytest.mark.smoke
def test_tone_16bit(device, services_before, record_property):
    """A four-second tone through the 16-bit mode (its default output, HDMI0)."""
    r = tone(device, MODE_16BIT)
    record_property("mode", r.kv.get("ahi.name"))
    record_property("stop_us", r.kv.get("ahi.stop_us"))


def test_driver_log_lines(device, driver_log, services_before, record_property):
    """The driver's own lines for one play: it starts, opens its AUDS service and stops; no trouble."""
    mark = driver_log.mark()
    tone(device, MODE_16BIT, 2000)
    log = driver_log.since(mark)
    lines = log.lines(r"^\[videocore\.audio\] (?:start|stop|AUDS open)")
    record_property("driver", "; ".join(lines))
    assert lines, "no driver lines in the debug log"
    log.forbid(TROUBLE, "the driver's log")


def test_tone_hifi(device, services_before):
    """The same through the HiFi mode."""
    tone(device, MODE_HIFI)


def test_two_streams(device, services_before):
    """Two plays at once: two streams, each its own AUDS service."""
    bg = device.start("VCTest", f"AHITONE MODE {MODE_16BIT} MS 6000 HZ 660")
    time.sleep(1)
    try:
        tone(device, MODE_16BIT)
    finally:
        rb = bg.wait(30)
    assert rb.passed, rb.kv.get("fail")


def test_break_stops_at_once(device, services_before, record_property):
    """A ten-second tone broken after two: it stops at once, and the next play works."""
    job = device.start("VCTest", f"AHITONE MODE {MODE_16BIT} MS 10000")
    time.sleep(2)
    job.stop()
    r = job.wait(10)
    assert r.passed and r.flag("ahi.stopped"), f"{r.kv.get('fail')} (stopped {r.kv.get('ahi.stopped')})"
    assert r.num("ahi.us") < 4_000_000, f"played {r.num('ahi.us') / 1e6:.1f} s after the break"
    record_property("stop_us", r.kv.get("ahi.stop_us"))
    assert r.num("ahi.stop_us") < 500_000, f"the stop took {r.num('ahi.stop_us') / 1000:.0f} ms"
    tone(device, MODE_16BIT)


def test_71_mode(device, services_before):
    """The 7.1 mode: a stereo sound into an eight-channel stream to the VPU."""
    tone(device, MODE_71)


@pytest.mark.listen
def test_71_order_by_ear(device, operator):
    """HDMI0 through the 7.1 mode: beeps 1 to 8 from front L, R, rear L, R, side L, R, centre, LFE."""
    operator.say("listen: eight groups of beeps, one speaker each, front left first")
    r = vctest(device, f"AHIWALK MODE {MODE_71}", 90)
    assert r.passed, r.kv.get("fail")
    operator.ask("Did beeps 1-8 come from front L, front R, rear L, rear R, side L, side R, centre, LFE?")


@pytest.mark.smoke
def test_services_closed(device, services_before):
    """Every AUDS service the plays opened is closed again; nothing left on the codec."""
    assert_clean(status(device), services=services_before)
