# SPDX-License-Identifier: BSD-3-Clause
"""Where a frame's time goes: the perf probes of a profile-tier build, read from the debug log (developer-guide 8.6).

Six runs of three hundred frames: the decoder alone, full screen paced and unpaced, a 1600x900 and a default window,
full screen at 16 bits.  Each slot's average and the run's rate go to the baselines.
"""
import pytest

from harness.perf import slots

pytestmark = pytest.mark.profile

CASES = {  # case: (rig, arguments, the rate's key)
    "decoder": ("VCTest", "DECODE {clip} FRAMES 300 NOHASH", "decode.fps"),
    "fullscreen": ("VPUPlay", "{clip} FRAMES 300", "play.fps"),
    "fullscreen-fast": ("VPUPlay", "{clip} FRAMES 300 FAST", "play.fps"),
    "window-1600x900": ("VPUPlay", "{clip} FRAMES 300 WINDOW WIDTH 1600 HEIGHT 900", "play.fps"),
    "window": ("VPUPlay", "{clip} FRAMES 300 WINDOW", "play.fps"),
    "fullscreen-16bit": ("VPUPlay", "{clip} FRAMES 300 DEPTH 16", "play.fps"),
}


@pytest.mark.parametrize("case", list(CASES), ids=list(CASES))
def test_timing(device, material, debuglog, baselines, record_property, case):
    """One run: its rate, and every probe's average over it."""
    rig, args, rate_key = CASES[case]
    mark = debuglog.mark()
    r = device.run(rig, args.format(clip=material.ram("clip42.h264")), 120)
    assert r.passed, r.kv.get("fail")
    baselines.check(f"profile.{case}.fps", r.num(rate_key), higher=True, tolerance="10%")
    text = debuglog.since(mark).text
    found = {prefix: slots(text, prefix) for prefix in ("videocore", "vchiq")}
    assert any(found.values()), "no perf lines in the log: not a profile build, or the wrong adapter"
    for prefix, table in found.items():
        for name, slot in sorted(table.items()):
            if slot.n == 0:
                continue
            record_property(f"{prefix}.{name}", f"n={slot.n} avg={slot.avg_us:.1f}us max={slot.max_us}us")
            baselines.check(f"profile.{case}.{prefix}.{name}.avg_us", round(slot.avg_us, 1), higher=False, tolerance="25%")
