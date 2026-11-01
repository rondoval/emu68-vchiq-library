# SPDX-License-Identifier: BSD-3-Clause
"""The missing codec drawer, resets with the device up, and the repair of a session left as a crash would."""
import time

import pytest

from harness.results import expect
from harness.tools import codec_memory, status, vctest
from test_ahi import MODE_16BIT

PRESS_WAIT_S = 1800  # someone has to come to the machine: longer than a test's limit, so these have their own


def test_missing_drawer(device):
    """No LIBS:videocodec drawer: no codecs, no hang; the drawer back, the codecs back."""
    device.flush()
    device.exec("Rename LIBS:videocodec AS LIBS:videocodec-tests", timeout=15)
    try:
        gone = vctest(device, "CODECS", 30)
    finally:
        device.exec("Rename LIBS:videocodec-tests AS LIBS:videocodec", timeout=15)
    assert not gone.passed and gone.num("codecs") == 0, f"{gone.kv.get('codecs')} codecs without the drawer"
    device.flush()  # the scan is cached until the library goes
    back = vctest(device, "CODECS", 30)
    assert back.passed and back.num("codecs") == 11, f"{back.kv.get('codecs')} codecs with the drawer back"


@pytest.mark.reboot
def test_reboot_resumes(device, driver_log):
    """C:Reboot with the device up: the reset guard quiesces, the next open resumes the session."""
    status(device)
    mark = driver_log.mark()
    device.reboot()
    r = status(device)
    assert r.kv.get("vchiq.bringup") == "resumed", f"bringup {r.kv.get('vchiq.bringup')}"
    driver_log.since(mark).expect(r"connection live, quiesced", "the device's log")


def a_take_runs(device, total):
    """After a reset with the VideoCore at work: no memory stays gone, and a take runs. The bring-up it found."""
    r = status(device)
    free, _ = codec_memory(r)
    assert free >= total - (8 << 20), f"{free >> 20} of {total >> 20} MiB free: the VPU kept what the reset left running"
    screen = device.start("VCTest", "SCREEN WIDTH 1920 HEIGHT 1080 DEPTH 32 SECONDS 12")
    time.sleep(3)
    try:
        rc, text = device.run_text("VCGrab", "RAM:hw/after.mkv SECONDS 3", 30)
    finally:
        screen.stop()
    assert rc == 0 and "no end mark" not in text, f"the take after the reset: rc {rc}: {text}"
    device.delete("RAM:hw/after.mkv")
    return r.kv.get("vchiq.bringup")


def after_a_reset(device, driver_log):
    """C:Reboot with the VideoCore at work: the session comes back, a take runs on it, and no memory stays gone."""
    _, total = codec_memory(status(device))
    mark = driver_log.mark()
    device.reboot()
    bringup = status(device).kv.get("vchiq.bringup")
    assert bringup in ("resumed", "repaired"), f"bringup {bringup}: {driver_log.since(mark).text[-800:]}"
    return a_take_runs(device, total)


def external_reset(device, operator):
    """Ctrl-Amiga-Amiga by hand, and the machine back."""
    operator.say(f"press Ctrl-Amiga-Amiga on the Amiga now (waiting up to {PRESS_WAIT_S // 60} minutes)")
    assert device.wait_reset(PRESS_WAIT_S), f"no reset seen in {PRESS_WAIT_S // 60} minutes"


@pytest.mark.reboot
def test_reboot_during_a_take(device, driver_log, record_property):
    """C:Reboot with the ISP and the encoder tunnelled and running: nothing of them stays on the VPU to spoil the next take."""
    screen = device.start("VCTest", "SCREEN WIDTH 1920 HEIGHT 1080 DEPTH 32 SECONDS 30")
    time.sleep(3)
    device.start("VCGrab", "RAM:hw/reset.mkv")
    time.sleep(5)
    assert device.procs("VCGrab"), f"the take did not start:\n{screen.output()}"
    record_property("bringup", after_a_reset(device, driver_log))


@pytest.mark.reboot
def test_reboot_during_playback(device, material, driver_log, record_property):
    """C:Reboot with a video playing, the decoder tunnelled into the ISP: the same."""
    player = device.start("VPUPlay", f"{material.ram('clip42.h264')} LOOP")
    time.sleep(5)
    assert device.procs("VPUPlay"), f"the player did not start:\n{player.output()}"
    record_property("bringup", after_a_reset(device, driver_log))


@pytest.mark.reboot
def test_reboot_during_sound(device, driver_log, record_property):
    """C:Reboot with AHI playing through videocore.audio: the same."""
    tone = device.start("VCTest", f"AHITONE MODE {MODE_16BIT} MS 20000")
    time.sleep(3)
    assert device.procs("VCTest"), f"the tone did not start:\n{tone.output()}"
    record_property("bringup", after_a_reset(device, driver_log))


@pytest.mark.reboot
@pytest.mark.hooks
def test_abandon_then_repair(device, driver_log):
    """A session left as a crash would (ABANDON, then C:Reboot): the first open after the reset repairs it, and the
    repaired connection carries a service with bulk transfers (the audio service); a reboot leaves it resumed."""
    left = vctest(device, "ABANDON", 30)
    assert left.passed and left.flag("abandon.armed"), left.kv.get("fail")
    mark = driver_log.mark()
    device.reboot()
    try:
        r = status(device)
        assert r.kv.get("vchiq.bringup") == "repaired", f"bringup {r.kv.get('vchiq.bringup')}"
        rc, text = device.run_text("VCInfo", "GCMD version")
        expect(text, r"^PASS$", "gencmd after the repair")
        audio = vctest(device, "AUDIO MS 500", 60)
        assert audio.passed, f"the audio service on the repaired connection: {audio.kv.get('fail')} (a VPU that stops answering " \
                             f"needs a power cycle)"
        log = driver_log.since(mark)
        for regex in (r"reset: left as a crash would", r"connection live, dirty", r"repair of the previous session: ok"):
            log.expect(regex, "the device's log")
    finally:
        device.reboot()  # the repaired session is not left behind for the next run: resumed, as every run starts


@pytest.mark.hands
@pytest.mark.timeout(PRESS_WAIT_S + 300)
@pytest.mark.reboot
@pytest.mark.hooks
def test_abandon_then_external_reset(device, operator, driver_log):
    """A session left as a crash would, then Ctrl-Amiga-Amiga, which restarts the Pi while DRAM keeps the record: the
    device finds a dirty record the VPU has forgotten, and starts afresh instead of failing until the record decays."""
    left = vctest(device, "ABANDON", 30)
    assert left.passed and left.flag("abandon.armed"), left.kv.get("fail")
    mark = driver_log.mark()
    external_reset(device, operator)
    r = status(device)
    assert r.kv.get("vchiq.bringup") in ("reinit", "repaired"), f"bringup {r.kv.get('vchiq.bringup')}"
    audio = vctest(device, "AUDIO MS 500", 60)
    assert audio.passed, f"the audio service after the reset: {audio.kv.get('fail')}"
    driver_log.since(mark).expect(r"re-initialised|repair of the previous session: ok", "the device's log")


@pytest.mark.hands
@pytest.mark.timeout(PRESS_WAIT_S + 300)
@pytest.mark.reboot
def test_external_reset(device, operator, capture, record_property):
    """Ctrl-Amiga-Amiga by hand: the device comes back, resumed or started afresh."""
    mark = capture.mark()
    external_reset(device, operator)
    rc, text = device.run_text("VCInfo", "GCMD version")
    expect(text, r"^PASS$", "gencmd after the reset")
    if capture.ok:
        log = capture.since(mark)
        path = ("the Pi restarted, the record re-initialised" if log.find(r"record was stale|connection fresh") else
                "resumed" if log.find(r"connection live, quiesced") else "unknown")
        record_property("after the reset", path)


@pytest.mark.hands
@pytest.mark.timeout(PRESS_WAIT_S + 300)
@pytest.mark.reboot
def test_external_reset_during_a_take(device, operator, record_property):
    """Ctrl-Amiga-Amiga with the ISP and the encoder tunnelled and running: the reset comes in an interrupt, where the
    codec's guard can do nothing, and restarts the Pi. The next take runs, with all the VPU's memory back."""
    _, total = codec_memory(status(device))
    device.start("VCTest", "SCREEN WIDTH 1920 HEIGHT 1080 DEPTH 32 SECONDS 600")
    time.sleep(3)
    device.start("VCGrab", "RAM:hw/reset.mkv")
    time.sleep(5)
    assert device.procs("VCGrab"), "the take did not start"
    external_reset(device, operator)
    record_property("bringup", a_take_runs(device, total))
