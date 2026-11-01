# SPDX-License-Identifier: BSD-3-Clause
"""Sessions with the RAM the VPU reaches nearly gone, held on purpose: at the top of its block, and failing as they open.

What is left of the largest free stretch sits at the block's end, where Emu68 once stopped on a cache range that ran
past it, once in many runs of everything else.  Each level runs a short job of every kind: whatever fits must keep
inside its block, whatever does not must fail cleanly, and neither may leave anything on the VPU.
"""
import re
import time
from contextlib import contextmanager

import pytest

from harness.tools import assert_nothing_left, codec_memory, derived, status, vctest
from test_encoder import H, W, bars

MIB = 1 << 20
REACH_END = 1 << 30  # the VPU reaches the lowest GiB
CHIP_END = 2 * MIB
LEAVE_MIB = (64, 32, 16, 12, 8, 6, 4, 3, 2)
BACK_SLACK = 6 * MIB  # the codec memory may wander this much between two readings
OBJECT = re.compile(r"\[videocore\] (?:frame|packet) at ([0-9a-f]{8}), (\d+) bytes")

FREE = (  # every free chunk of a MiB or more that the VPU reaches, as bytes:start:its block's end
    "/**/ numeric digits 12; call addlib('rexxsupport.library',0,-30,0); sb=c2d(import('00000004'x,4)); call forbid(); "
    "ml=c2d(import(d2c(sb+322,4),4)); out=''; "
    "do forever; nxt=c2d(import(d2c(ml,4),4)); if nxt=0 then leave; upper=c2d(import(d2c(ml+24,4),4)); "
    f"if upper<={REACH_END} & upper>{CHIP_END} then do; c=c2d(import(d2c(ml+16,4),4)); "
    "do while c~=0; b=c2d(import(d2c(c+4,4),4)); if b>=1048576 then out=out b':'c':'upper; "
    "c=c2d(import(d2c(c,4),4)); end; end; ml=nxt; end; call permit(); return out"
)

def free_chunks(device):
    """Every free chunk the VPU reaches, largest first: (bytes, start, its block's end)."""
    rc, text = device.ami.arexx(FREE)
    assert rc == 0, f"ARexx: {rc}: {text}"
    return sorted((tuple(map(int, w.split(":"))) for w in text.split()), reverse=True)


@contextmanager
def held(device, leave):
    """All the free RAM the VPU reaches taken but @leave MiB, at the end of the largest chunk: where its block ends."""
    device.flush()  # the DMA pool keeps arenas between sessions: each level asks for its memory afresh
    best, _, top = free_chunks(device)[0]
    assert best > (leave + 64) * MIB, f"only {best >> 20} MiB free in reach: a run before this left too little"
    job = device.start("VCTest", f"HOLD LEAVE {leave}")
    try:
        t0 = time.time()
        while sum(size for size, _, _ in free_chunks(device)) > (leave + 1) * MIB:  # a hold that falls short proves nothing
            assert time.time() - t0 < 10, f"the hold left {[(hex(at), size >> 20) for size, at, _ in free_chunks(device)]} MiB"
            time.sleep(0.2)
        yield top
    finally:
        job.stop()
        device.delete("RAM:hw/lm.out")


def jobs(device, material):
    """One short job of every kind that keeps pictures in the RAM the VPU reaches: (name, VCTest's arguments)."""
    clip = derived(device, material, "clip.i420")
    return (
        ("encoder", f"ENCODE {clip} WIDTH 1920 HEIGHT 1080 FPS 30 BITRATE 4000000 KEYINT 10 OUT RAM:hw/lm.out"),
        ("through the ISP", f"ENCODE {bars(device, 'BGRA32')} WIDTH {W} HEIGHT {H} FORMAT BGRA32 FPS 30 BITRATE 4000000 KEYINT 5 OUT RAM:hw/lm.out"),
        ("decoder", f"DECODE {material.ram('clip42.h264')} FRAMES 30 NOHASH"),
        ("image", f"IMAGE {material.ram('stills/j6000.jpg')} OUT RAM:hw/lm.out OUTFORMAT RGB24"),
    )


@pytest.fixture(scope="module")
def baseline(device):
    """The codec memory free with nothing running: a VPU that starts short says so here, not as a leak."""
    free, total = codec_memory(status(device))
    assert free >= total - 2 * BACK_SLACK, f"{free >> 20} of {total >> 20} MiB free before the first job: restart the Pi"
    return free


@pytest.mark.parametrize("leave_mib", LEAVE_MIB)
def test_jobs_with_little_left(device, material, debuglog, baseline, record_property, leave_mib):
    """Every job with this much left: inside the block or refused, and the VPU's memory back after each."""
    work = jobs(device, material)
    mark = debuglog.mark()
    outcome = []
    with held(device, leave_mib) as top:
        for name, args in work:
            r = vctest(device, args, 60)
            outcome.append(f"{name}: {'ran' if r.passed else 'refused'}")
            free, _ = codec_memory(status(device))
            assert free >= baseline - BACK_SLACK, f"{name} left {(baseline - free) >> 20} MiB on the VPU ({outcome}): {r.kv.get('fail')}"
    record_property("jobs", "; ".join(outcome))
    log = debuglog.since(mark)  # empty on a release build, which still has every check above
    placed = [(int(at, 16), int(size)) for at, size in OBJECT.findall(log.text)]
    record_property("highest end", hex(max((at + size for at, size in placed), default=0)))
    past = [(hex(at), size) for at, size in placed if at < top < at + size]
    assert not past, f"past the block's end at {top:#x}: {past}"
    log.forbid(r"page fault", "Emu68's log")
    log.forbid(r"did not answer", "the codec's log")


def test_nothing_left(device, baseline):
    """With the memory given back: no session left, and a whole job runs as ever."""
    assert_nothing_left(device)
    free, _ = codec_memory(status(device))
    assert free >= baseline - BACK_SLACK, f"{(baseline - free) >> 20} MiB left on the VPU"
