# SPDX-License-Identifier: BSD-3-Clause
"""The firmware's audio service (AUDS), driven directly by VCTest AUDIO: what videocore.audio is built on."""
import time

import pytest

from harness.tools import assert_nothing_left, vctest

RATES = (8000, 11025, 16000, 22050, 32000, 44100, 48000)
HDMI0, JACK, HDMI1 = 2, 1, 3
DESTS = {"hdmi0": HDMI0, "jack": JACK, "hdmi1": HDMI1}


def audio(device, args, timeout=60):
    """VCTest AUDIO with @args, which must pass."""
    r = vctest(device, f"AUDIO {args}", timeout)
    assert r.passed, f"{r.kv.get('fail')} (CONTROL {r.kv.get('control.result')}, CONFIG {r.kv.get('config.result')})"
    return r


def rate_params():
    """Every rate on HDMI0 and the jack; 48 kHz on HDMI0 is the smoke case."""
    return [pytest.param(dest, rate, id=f"{dest}-{rate}", marks=pytest.mark.smoke if (dest, rate) == ("hdmi0", 48000) else [])
            for dest in ("hdmi0", "jack") for rate in RATES]


@pytest.mark.parametrize("dest,rate", rate_params())
def test_rates(device, record_property, dest, rate):
    """Every rate the driver offers: CONFIG taken, the samples consumed at the rate (within 3 percent)."""
    r = audio(device, f"MS 2000 DEST {DESTS[dest]} RATE {rate} CHUNK 10 DEPTH 4")
    consumed = r.num("run.0.consumed_fps")
    record_property("consumed", f"{consumed}/s, {r.kv.get('run.0.flagged')} flagged")
    assert abs(consumed - rate) <= rate * 0.03, f"consumed {consumed} frames/s at {rate} Hz"


@pytest.mark.parametrize("chunk,depth", [(10, d) for d in (1, 2, 3, 4, 6, 8)] + [(20, d) for d in (1, 2, 3, 4)],
                         ids=lambda v: str(v))
def test_chunk_and_depth(device, record_property, chunk, depth):
    """Chunk size and queue depth on HDMI0 at 48 kHz: underruns, the first COMPLETE, the longest gap and bulk."""
    r = audio(device, f"MS 3000 CHUNK {chunk} DEPTH {depth}")
    for key in ("flagged", "first_us", "gap_us", "bulk_us"):
        record_property(key, r.kv.get(f"run.0.{key}"))


def test_stop_and_restart(device, record_property):
    """STOP and START again, then STOP with four chunks queued: what STOP leaves of them."""
    r = audio(device, "MS 1000 CHUNK 10 DEPTH 4 RESTART")
    stops = r.group("stop")
    assert len(stops) == 2, f"{len(stops)} STOPs"
    record_property("drained", stops[0])
    record_property("queued", stops[1])
    record_property("answers not waited for", r.kv.get("audio.unexpected"))


@pytest.mark.parametrize("dest", ["hdmi0", "jack"], ids=["both-hdmi0", "hdmi0-and-jack"])
def test_two_streams(device, record_property, dest):
    """Two streams at once: a second AUDIO beside a running one, both on HDMI0, then HDMI0 and the jack."""
    bg = device.start("VCTest", "AUDIO MS 4000 CHUNK 10 DEPTH 4")
    time.sleep(1)
    try:
        r = audio(device, f"MS 2000 DEST {DESTS[dest]} CHUNK 10 DEPTH 4")
    finally:
        rb = bg.wait(30)
    assert rb.passed, rb.kv.get("fail")
    record_property("flagged", f"{rb.kv.get('run.0.flagged')} and {r.kv.get('run.0.flagged')}")


def test_hdmi1(device, record_property):
    """HDMI1, usually with nothing on it: how the service answers (recorded, a fact of the machine)."""
    r = vctest(device, f"AUDIO MS 500 DEST {HDMI1} CHUNK 10 DEPTH 4", 60)
    record_property("outcome", "PASS" if r.passed else f"{r.kv.get('fail')} (CONTROL {r.kv.get('control.result')}, "
                                                       f"CONFIG {r.kv.get('config.result')})")


@pytest.mark.parametrize("dest,channels", [("hdmi0", 1), ("hdmi0", 4), ("hdmi0", 6), ("hdmi0", 8), ("hdmi1", 8), ("jack", 8)],
                         ids=lambda v: str(v))
def test_channels(device, record_property, dest, channels):
    """Channel counts at 48 kHz: each refused by CONFIG (recorded) or consumed at the rate."""
    r = vctest(device, f"AUDIO MS 2000 DEST {DESTS[dest]} CHANNELS {channels} CHUNK 10 DEPTH 4", 60)
    if r.kv.get("config.result") not in (None, "0"):
        record_property("CONFIG", f"refused: {r.kv['config.result']}")
        return
    assert r.passed, r.kv.get("fail")
    consumed = r.num("run.0.consumed_fps")
    record_property("consumed", f"{consumed}/s, {r.kv.get('run.0.flagged')} flagged")
    assert abs(consumed - 48000) <= 48000 * 0.03, f"consumed {consumed} frames/s"


# -- by ear

@pytest.mark.listen
def test_tone_and_volume_step(device, operator):
    """HDMI0: a 1 kHz tone for two seconds, then the same 20 dB quieter."""
    operator.say("listen to HDMI0: a tone, then the same tone quieter")
    audio(device, "MS 2000 TONE CHUNK 10 DEPTH 4")
    time.sleep(1)
    audio(device, "MS 2000 TONE VOL 5120 CHUNK 10 DEPTH 4")
    operator.ask("Did you hear a two-second tone, then the same tone about 20 dB quieter?")


@pytest.mark.listen
def test_tone_on_jack(device, operator, record_property):
    """The headphone jack: the same two tones; its pins' functions recorded."""
    operator.say("listen to the headphone jack: a tone, then the same tone quieter")
    r = audio(device, f"MS 2000 DEST {JACK} TONE CHUNK 10 DEPTH 4")
    record_property("jack pins", f"{r.kv.get('pins.before')} before, {r.kv.get('pins.playing')} playing")
    time.sleep(1)
    audio(device, f"MS 2000 DEST {JACK} TONE VOL 5120 CHUNK 10 DEPTH 4")
    operator.ask("Did the jack play a two-second tone, then the same tone about 20 dB quieter?")


@pytest.mark.listen
def test_tone_at_three_rates(device, operator):
    """HDMI0: the 1 kHz tone at 22050, 44100 and 48000 Hz, one second each: the same pitch three times."""
    operator.say("listen to HDMI0: the same tone three times")
    for rate in (22050, 44100, 48000):
        audio(device, f"MS 1000 TONE RATE {rate} CHUNK 10 DEPTH 4")
        time.sleep(1)
    operator.ask("Was it the same pitch all three times?")


@pytest.mark.listen
def test_channel_walk(device, operator):
    """HDMI0: left then right for reference, then eight channels, channel n beeping n times."""
    operator.say("listen to HDMI0: left once, right twice; then channels 1 to 8, channel n beeping n times")
    audio(device, "CHANNELS 2 WALK CHUNK 10 DEPTH 4", 60)
    time.sleep(3)
    audio(device, "CHANNELS 8 WALK CHUNK 10 DEPTH 4", 90)
    operator.ask("Did each group of beeps come from a different speaker, in a sensible order?")


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left, no stray message."""
    assert_nothing_left(device)
