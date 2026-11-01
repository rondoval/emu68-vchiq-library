# SPDX-License-Identifier: BSD-3-Clause
"""VCGrab: a screen through an encoder session into a file, judged on the host from what it wrote."""
import os
import re
import struct
import subprocess
import time
from contextlib import contextmanager

import numpy as np
import pytest

from harness.material import ffmpeg, sh
from harness.pictures import BAR_COLOURS, judge_bars, key_frames, pattern, raw_bars
from harness.tools import ZC_TROUBLE, assert_nothing_left, dt_page, judge_screen_bars, status, vctest
from test_ahi import MODE_16BIT
from unit.test_mkv import CLUSTER, CUES, File

W, H = 1920, 1080
READINGS = [("bt601", False), ("bt601", True), ("bt709", False), ("bt709", True)]
ISP_READING = "bt601/full"  # what the ISP makes of RGB, whatever colour space its ports are given
REPORT = re.compile(r"(\d+) pictures, (\d+)x(\d+), (\d+) ms, (\d+) ticks dropped, (\d+) paused, (\d+) bytes, 68k busy (\d+)%, (\d+) MiB free")
KEYS = ("pictures", "width", "height", "ms", "dropped", "paused", "bytes", "busy", "free")
CTRL_ALT = 0x0008 | 0x0010  # IEQUALIFIER_CONTROL, IEQUALIFIER_LALT
KEY_R, KEY_S, KEY_G, KEY_F, KEY_H, KEY_V = 0x13, 0x21, 0x24, 0x23, 0x25, 0x34  # raw key codes
PREFS = "ENV:VCGrab.prefs"
DISK = os.environ.get("VCGRAB_DISK")  # a volume with room, "Stuff:": the tests that write to a disk
PAST_2_GIB_SECONDS = 800  # noise fills 3.1 MB/s
UP_TO_4_GIB_SECONDS = 1500  # long enough for the file limit to end it
TAKE_ACROSS_WRAP = 120
TIMER_WRAP = 1 << 32  # microseconds: the Pi's timer, get_time()
FULL_DISK_MIB = 48


@pytest.fixture(autouse=True)
def no_saved_settings(device):
    """Every test starts with nothing saved, or a stale file would change the commodity's keys."""
    device.delete(PREFS)
    yield
    device.delete(PREFS)


def no_trouble(log):
    """None of the import trouble lines in a log slice."""
    for t in ZC_TROUBLE:
        log.forbid(r"\[videocore\] .*" + t, "the codec's log")


def mode(device, width=W, height=H, depth=32):
    """What a screen of this size and depth comes to, briefly opened: VCTest's result; skips where the Amiga has no such mode."""
    r = vctest(device, f"SCREEN WIDTH {width} HEIGHT {height} DEPTH {depth} SECONDS 1", 30)
    if not r.passed:
        pytest.skip(f"no RTG mode of {width}x{height} at {depth} bits: {r.kv.get('fail')}")
    return r


@contextmanager
def showing(device, width=W, height=H, depth=32, extra="", seconds=300):
    """The bars on a screen of their own, in front, for as long as the block runs: @seconds at most."""
    job = device.start("VCTest", f"SCREEN WIDTH {width} HEIGHT {height} DEPTH {depth} SECONDS {seconds} {extra}")
    time.sleep(3)
    assert device.procs("VCTest"), f"the screen did not stay up:\n{job.output()}"
    try:
        yield job
    finally:
        job.stop()


def hot_key(device, key):
    """Control-Alt and a raw @key, as if pressed: commodities see it as they see the keyboard."""
    device.ami.input_key(key, qualifiers=CTRL_ALT)
    time.sleep(0.5)


def numbers_of(text):
    """The report's numbers, None without a report."""
    m = REPORT.search(text)
    return dict(zip(KEYS, map(int, m.groups()))) if m else None


def fetch(device, artifacts, remote, name):
    """A file off the Amiga into the artifacts, gone from the Amiga: its local path."""
    local = artifacts / name
    local.write_bytes(device.download(remote))
    device.delete(remote)
    return local


def grab(device, artifacts, name, args, timeout=60):
    """VCGrab to a RAM: file, fetched into the artifacts: (rc, text, its numbers, local path)."""
    remote = f"RAM:hw/{name}"
    rc, text = device.run_text("VCGrab", f"{remote} {args}", timeout)
    (artifacts / f"{name}.txt").write_text(text)
    numbers = numbers_of(text)
    local = fetch(device, artifacts, remote, name) if numbers and numbers["pictures"] else None
    return rc, text, numbers, local


def rgb_picture(path, picture, width, height):
    """Picture number @picture as a player shows it: through the stream's own colour tags, to RGB."""
    data = ffmpeg("-i", str(path), "-vf", f"select=eq(n\\,{picture})", "-vframes", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-")
    return np.frombuffer(data[:width * height * 3], np.uint8).reshape(height, width, 3)


def stream_fields(path):
    """What ffprobe says of the stream and the file."""
    out = sh(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries",
              "stream=codec_name,profile,level,width,height,color_range,color_space:format=duration", "-of", "default=nw=1", str(path)])
    return dict(line.split("=", 1) for line in out.decode().splitlines())


def decode_errors(path, *before):
    """What ffmpeg says against a file as it decodes all of it, or from @before's -ss on; ffmpeg() drops that and looks only at the exit code."""
    return subprocess.run(["ffmpeg", "-nostdin", "-v", "error", *before, "-i", str(path), "-f", "null", "-"], capture_output=True).stderr.decode()


def packet_times(path):
    """Each packet's time in ms."""
    out = sh(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "packet=pts_time", "-of", "csv=p=0", str(path)])
    return [round(float(line.strip().rstrip(",")) * 1000) for line in out.decode().split()]


def bar_yuv(y, u, v, width, height):
    """Each bar's mean Y, U and V, from the middle of the bar."""
    _, bars = pattern(width, height)
    out = []
    for b in bars:
        x0, x1 = b.x + b.w // 4, b.x + b.w * 3 // 4
        y0, y1 = b.y + b.h // 4, b.y + b.h * 3 // 4
        out.append((y[y0:y1, x0:x1].mean(), u[y0 // 2:y1 // 2, x0 // 2:x1 // 2].mean(), v[y0 // 2:y1 // 2, x0 // 2:x1 // 2].mean()))
    return out


def predicted(rgb, matrix, full):
    """What a colour comes to under one reading: (Y, U, V)."""
    kr, kb = (0.299, 0.114) if matrix == "bt601" else (0.2126, 0.0722)
    r, g, b = (c / 255 for c in rgb)
    y = kr * r + (1 - kr - kb) * g + kb * b
    u, v = (b - y) / (2 * (1 - kb)), (r - y) / (2 * (1 - kr))
    return (255 * y, 128 + 255 * u, 128 + 255 * v) if full else (16 + 219 * y, 128 + 224 * u, 128 + 224 * v)


def reading_of(got):
    """Which reading some bars' YUV fit best, and the largest error of any bar under it: (name, error, each reading's error)."""
    errors = {}
    for matrix, full in READINGS:
        worst = max(max(abs(g - p) for g, p in zip(bar, predicted(rgb, matrix, full))) for bar, rgb in zip(got, BAR_COLOURS))
        errors[f"{matrix}/{'full' if full else 'limited'}"] = round(float(worst), 1)
    best = min(errors, key=errors.get)
    return best, errors[best], errors


def long_take(device, artifacts, record_property, remote, seconds):
    """Noise to a real disk for @seconds, or until the file limit: (the report, its numbers, the file's start, where its Cues are)."""
    with showing(device, extra="NOISE", seconds=seconds + 300):
        job = device.start("VCGrab", f"{remote} BITRATE 25000 SECONDS {seconds}")
        t0 = time.time()
        while device.procs("VCGrab") and time.time() - t0 < seconds + 120:
            device.wake()  # the screen blanker must not come in front
            time.sleep(10)
        text = job.wait(60)
    record_property("report", text.strip())
    n = numbers_of(text)
    assert n is not None and n["paused"] == 0, f"the screen left the front: {text}"
    head = first_bytes(device, remote, 1024)
    (artifacts / "head.bin").write_bytes(head)
    start = File(head[:head.index(struct.pack(">I", CLUSTER))])
    cues_at = start.seek_targets()[CUES]
    record_property("Duration, Cues at", (start.duration(), cues_at))
    return text, n, start, cues_at


@contextmanager
def small_ffs(device):
    """A small empty FFS volume in memory (RAD:) for as long as the block runs, what a disk filling up needs: its name."""
    cylinders = (FULL_DISK_MIB << 20) // (2 * 11 * 512)
    device.upload_bytes("RAM:hw/RAD", (f"Device = ramdrive.device\nUnit = 0\nFlags = 0\nSurfaces = 2\nSectorsPerTrack = 11\nSectorSize = 512\n"
                                       f"Reserved = 2\nInterleave = 0\nBuffers = 32\nBufMemType = 1\nLowCyl = 0\nHighCyl = {cylinders - 1}\n"
                                       f"Activate = 1\n").encode())
    rc, text = device.exec("Mount RAM:hw/RAD", 20)
    assert rc == 0, f"Mount: {text}"
    try:
        rc, text = device.exec("SYS:System/Format <NIL: DRIVE RAD: NAME VCGrabFull FFS NOICONS QUICK", 60)
        assert rc == 0, f"Format: {text}"
        yield "VCGrabFull:"
    finally:
        device.exec("RemRAD FORCE", 20)  # first: a recoverable drive outlives a reset, and needs its handler to go
        device.exec("Assign RAD: DISMOUNT", 20)


def first_bytes(device, remote, length):
    """A file's first bytes, read in order: the agent's own reads ask for the size, which a file over 2 GiB cannot give.

    ARexx hands back little at a time, so each piece reads past the ones before it.
    """
    piece, data = 256, b""
    while len(data) < length:
        skip = f"x=readch('f',{len(data)});" if data else ""
        rc, text = device.ami.arexx(f"/**/ x=open('f','{remote}','R'); {skip} r=readch('f',{piece}); x=close('f'); return c2x(r)")
        assert rc == 0, f"ARexx: {rc}: {text}"
        data += bytes.fromhex(text.strip())
    return data[:length]


@pytest.mark.smoke
@pytest.mark.parametrize("depth", [16, 24, 32])
def test_records_the_bars(device, driver_log, artifacts, record_property, depth):
    """Five seconds of a 1080p screen: every tick taken, no copy, a file that says its colours, the bars right as a player shows them."""
    mode(device, depth=depth)
    with showing(device, depth=depth):
        mark = driver_log.mark()
        rc, text, n, mkv = grab(device, artifacts, f"bars{depth}.mkv", "SECONDS 5")
        log = driver_log.since(mark)
    assert rc == 0 and n is not None, f"rc {rc}: {text}"
    record_property("report", text.strip())
    log.expect(r"ril\.isp port 1: zero copy on", "the screen, read by the ISP itself")
    log.expect(r"encoder running behind the ISP", "the pictures, tunnelled inside the VPU")
    no_trouble(log)
    assert n["dropped"] <= 3, text
    frames, keys = key_frames(mkv)
    record_property("key frames", keys)
    assert frames >= 145 and keys[:3] == [0, 60, 120], f"{frames} pictures, key frames at {keys}"
    fields = stream_fields(mkv)
    record_property("stream", fields)
    assert fields["profile"] == "High" and fields["level"] == "40" and fields["color_range"] == "pc", fields
    assert abs(float(fields["duration"]) - frames / 30) < 0.2, fields
    _, bars = pattern(W, H)
    problems = judge_bars(rgb_picture(mkv, 30, W, H), bars)
    assert not problems, problems
    assert decode_errors(mkv) == "", "every picture decodes"
    assert decode_errors(mkv, "-ss", "3") == "", "seeking"


@pytest.mark.parametrize("fmt", ["RGB16", "RGB24", "BGR24", "BGRA32", "RGBA32"])
def test_isp_reads_each_format(device, artifacts, record_property, fmt):
    """Each screen format, 1080 rows from memory, through the ISP into I420: its byte order right and every row taken."""
    raw, out = f"RAM:hw/bars.{fmt}", f"RAM:hw/bars-{fmt}.i420"
    device.upload_bytes(raw, raw_bars(fmt, W, H))
    r = vctest(device, f"CONVERT {raw} WIDTH {W} HEIGHT {H} FORMAT {fmt} OUTFORMAT I420 OUT {out}", 60)
    device.delete(raw)
    assert r.passed, f"{fmt}: {r.kv.get('fail')}, error {r.kv.get('error')}"
    data = device.download(out)
    device.delete(out)
    y = np.frombuffer(data[:W * H], np.uint8).reshape(H, W)
    cw, ch = W // 2, H // 2
    u = np.frombuffer(data[W * H:W * H + cw * ch], np.uint8).reshape(ch, cw)
    v = np.frombuffer(data[W * H + cw * ch:W * H + 2 * cw * ch], np.uint8).reshape(ch, cw)
    best, error, errors = reading_of(bar_yuv(y, u, v, W, H))
    record_property("colour reading", f"{best}, off by {error}: {errors}")
    assert best == ISP_READING and error < 8, f"{fmt}: the bars: {errors}"


def test_scaled(device, artifacts, record_property):
    """SIZE: a 1080p screen to 1280x720 through the ISP, the bars still right."""
    with showing(device):
        rc, text, n, mkv = grab(device, artifacts, "scaled.mkv", "SECONDS 3 SIZE 1280x720")
    assert rc == 0 and n is not None, f"rc {rc}: {text}"
    assert (n["width"], n["height"]) == (1280, 720)
    record_property("report", text.strip())
    fields = stream_fields(mkv)
    assert (fields["width"], fields["height"]) == ("1280", "720"), fields
    _, bars = pattern(1280, 720)  # the same bars scaled, near enough for their middles
    problems = judge_bars(rgb_picture(mkv, 30, 1280, 720), bars)
    assert not problems, problems


def test_width_needing_wider_rows(device, artifacts, record_property):
    """800 wide: the ISP's I420 rows are whole 64 pixels, so the frames are wider than the picture."""
    with showing(device):
        rc, text, n, mkv = grab(device, artifacts, "w800.mkv", "SECONDS 3 SIZE 800x450")
    assert rc == 0 and n is not None, f"rc {rc}: {text}"
    record_property("report", text.strip())
    fields = stream_fields(mkv)
    assert (fields["width"], fields["height"]) == ("800", "450"), fields
    frames, _ = key_frames(mkv)
    assert frames >= 80, text


def test_rows_it_cannot_read(device, artifacts, record_property):
    """A screen whose rows are no whole 32 pixels is refused in plain words, nothing left running, an earlier file of that name untouched."""
    r = mode(device, 1280, 720)
    pitch = r.num("screen.pitch")
    if (pitch // 4) % 32 == 0:
        pytest.skip(f"this screen's rows are whole 32 pixels: pitch {pitch}")
    earlier = b"an earlier recording"
    device.upload_bytes("RAM:hw/rows.mkv", earlier)
    with showing(device, 1280, 720):
        rc, text, n, _ = grab(device, artifacts, "rows.mkv", "SECONDS 2")
    record_property("report", text.strip())
    assert rc == 10 and "rows" in text and n is None, f"rc {rc}: {text}"
    assert device.download("RAM:hw/rows.mkv") == earlier, "the earlier file went"
    device.delete("RAM:hw/rows.mkv")


@pytest.mark.parametrize("args, code, words", [
    ("BITRATE 99999", 20, "BITRATE"),
    ("BITRATE 4294968", 20, "BITRATE"),
    ("SIZE 1280by720", 20, "SIZE"),
    ("SIZE 1x1", 20, "SIZE"),
    ("STILL SIZE 1x100", 20, "SIZE"),
    ("STILL SIZE 40000x40000", 20, "SIZE"),
    ("FPS 0", 20, "FPS"),
    ("SIZE 3840x2160", 10, "at most"),
    ("FPS 60", 10, "level 4.0"),
    ("SCREEN NoSuchScreen", 10, "public screen"),
], ids=["bitrate", "bitrate-wraps", "size-syntax", "size-odd", "still-size-small", "still-size-huge", "fps", "too-big", "too-fast", "no-screen"])
def test_refused_in_plain_words(device, args, code, words):
    """What cannot be done says why and leaves nothing behind."""
    with showing(device):
        rc, text = device.run_text("VCGrab", f"RAM:hw/refused.mkv SECONDS 1 {args}", 30)
    assert rc == code and words in text, f"rc {rc}: {text}"
    assert device.exec("C:List RAM:hw/refused.mkv", timeout=10)[0] != 0, "the file stayed"


@pytest.mark.parametrize("args, words", [
    (f"RAM:hw/{'a' * 300} SECONDS 1", "TO"),
    (f"RAM:hw/long CX STARTKEY {'k' * 70}", "hot key"),
    (f"RAM:hw/long CX CX_POPKEY {'k' * 70}", "hot key"),
], ids=["to", "key", "popkey"])
def test_too_long_is_refused(device, args, words):
    """Cut short it would name another file or key, so it is refused and nothing starts."""
    rc, text = device.run_text("VCGrab", args, 30)
    assert rc == 20 and words in text and "at most" in text, f"rc {rc}: {text}"


@pytest.mark.slow
def test_sustained(device, artifacts, record_property):
    """A minute of 1080p at 30 pictures a second: ticks dropped, the 68k's share, what the VPU has left."""
    with showing(device):
        rc, text, n, mkv = grab(device, artifacts, "minute.mkv", "SECONDS 60", 120)
    assert rc == 0 and n is not None, f"rc {rc}: {text}"
    record_property("report", text.strip())
    assert n["dropped"] <= 18, text
    frames, _ = key_frames(mkv)
    assert frames >= 1760, f"{frames} pictures: {text}"


@pytest.mark.slow
def test_worst_case_picture(device, artifacts, record_property):
    """Noise that changes all the time at the highest bit rate, into RAM:: every tick taken, and key frames too big for
    one packet arrive whole: they come in pieces of the same time and key flag, the last ending the picture."""
    with showing(device, extra="NOISE"):
        rc, text, n, mkv = grab(device, artifacts, "noise.mkv", "SECONDS 15 BITRATE 25000", 90)
    assert rc == 0 and n is not None, f"rc {rc}: {text}"
    record_property("report", text.strip())
    record_property("Mbit/s", round(n["bytes"] * 8 / (n["ms"] / 1000) / 1e6, 1))
    assert n["dropped"] <= 5 and "no end mark" not in text, text
    assert len(packet_times(mkv)) == n["pictures"], "every picture sent reached the file"
    sizes = [int(s.strip().rstrip(",")) for s in sh(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "packet=size", "-of", "csv=p=0", str(mkv)]).decode().split()]
    record_property("largest picture", max(sizes))
    assert max(sizes) > 512 << 10, "no picture needed more than one packet: the test proved nothing"
    assert decode_errors(mkv) == "", "every picture decodes"


@pytest.mark.slow
@pytest.mark.skipif(DISK is None, reason="VCGRAB_DISK names a volume to write to")
def test_worst_case_on_a_disk(device, artifacts, record_property):
    """The same to a real disk: what the writing costs in dropped ticks, the question of a writer process."""
    remote = f"{DISK}vcgrab-test.mkv"
    try:
        with showing(device, extra="NOISE"):
            rc, text = device.run_text("VCGrab", f"{remote} SECONDS 20 BITRATE 25000", 90)
    finally:
        device.delete(remote)
    record_property("report", text.strip())
    n = numbers_of(text)
    assert rc == 0 and n is not None, f"rc {rc}: {text}"
    record_property("dropped", f"{n['dropped']} of {n['pictures'] + n['dropped']}")


@pytest.mark.skipif(DISK is None, reason="VCGRAB_DISK names a volume to write to")
def test_an_ordinary_take_on_a_disk(device, artifacts, record_property):
    """A plain screen at the default bit rate to a real disk: what a normal recording asks of it."""
    remote = f"{DISK}vcgrab-test.mkv"
    try:
        with showing(device):
            rc, text = device.run_text("VCGrab", f"{remote} SECONDS 20", 60)
    finally:
        device.delete(remote)
    record_property("report", text.strip())
    n = numbers_of(text)
    assert rc == 0 and n is not None, f"rc {rc}: {text}"
    record_property("dropped", f"{n['dropped']} of {n['pictures'] + n['dropped']}")
    assert n["dropped"] <= 12, text  # the first send starts the session: most of these are that, whatever the disk


@pytest.mark.slow
@pytest.mark.skipif(DISK is None, reason="VCGRAB_DISK names a volume to write to")
@pytest.mark.timeout(3000)
def test_past_2_gib_on_a_disk(device, artifacts, record_property):
    """A real recording past 2 GiB on PFS3: the close seeks back from up there, and the Cues and Duration land right."""
    remote = f"{DISK}vcgrab-long.mkv"
    try:
        text, n, start, cues_at = long_take(device, artifacts, record_property, remote, PAST_2_GIB_SECONDS)
        assert n["bytes"] > 1 << 31, f"not past 2 GiB: {text}"
        assert start.duration() > 700 * 1000 and 1 << 31 < cues_at < n["bytes"], "the close's seek back from above 2 GiB landed"
    finally:
        device.delete(remote)


@pytest.mark.slow
@pytest.mark.skipif(DISK is None, reason="VCGRAB_DISK names a volume to write to")
@pytest.mark.timeout(3000)
def test_the_stop_near_4_gib(device, artifacts, record_property):
    """Noise until the file nears PFS3's 4 GiB: the take stops by itself and says so, and the file closes whole."""
    remote = f"{DISK}vcgrab-limit.mkv"
    try:
        text, n, start, cues_at = long_take(device, artifacts, record_property, remote, UP_TO_4_GIB_SECONDS)
        assert "nearly 4 GiB" in text, f"it did not stop on the limit: {text}"
        assert "no end mark" not in text and n["ms"] < UP_TO_4_GIB_SECONDS * 1000, text
        assert (1 << 32) - (64 << 20) < n["bytes"] < 1 << 32, f"stopped at the wrong place: {text}"
        assert start.duration() > 1000 * 1000 and 1 << 31 < cues_at < n["bytes"], "the close's seek back landed"
    finally:
        device.delete(remote)


@pytest.mark.slow
@pytest.mark.skipif(not os.environ.get("VCGRAB_WRAP"), reason="VCGRAB_WRAP=1: waits up to 72 minutes for the Pi's timer")
@pytest.mark.timeout(4800)
def test_the_clock_across_the_timers_wrap(device, artifacts, record_property):
    """A take that the Pi's timer wraps in the middle of, every 71.6 minutes: its clock goes on and its times stay regular."""
    left = (TIMER_WRAP - status(device).num("timer.now")) / 1e6
    record_property("seconds to the wrap", round(left))
    if left < TAKE_ACROSS_WRAP * 3 / 4:
        time.sleep(left + 5)
        left = TIMER_WRAP / 1e6
    while left > TAKE_ACROSS_WRAP / 2:
        time.sleep(min(600, left - TAKE_ACROSS_WRAP / 2))
        device.wake()
        left = (TIMER_WRAP - status(device).num("timer.now")) / 1e6
    with showing(device):
        job = device.start("VCGrab", f"RAM:hw/wrap.mkv SECONDS {TAKE_ACROSS_WRAP}")
        while device.procs("VCGrab"):
            device.wake()
            time.sleep(5)
        text = job.wait(60)
    record_property("report", text.strip())
    n = numbers_of(text)
    assert n is not None and n["paused"] == 0, f"the screen left the front: {text}"
    assert status(device).num("timer.now") < TIMER_WRAP / 2, "the timer did not wrap during the take"
    assert abs(n["ms"] - TAKE_ACROSS_WRAP * 1000) < 2000 and n["dropped"] <= 36, text
    times = packet_times(fetch(device, artifacts, "RAM:hw/wrap.mkv", "wrap.mkv"))
    gaps = [b - a for a, b in zip(times, times[1:])]
    record_property("pictures, largest gap in ms", (len(times), max(gaps)))
    assert min(gaps) > 0 and max(gaps) < 1000, "a jump in the file's times"


def test_a_full_disk(device, artifacts, record_property):
    """A small FFS volume filling up: the take ends in plain words, nothing hangs, and what reached the disk plays."""
    with small_ffs(device) as volume:
        remote = f"{volume}full.mkv"
        with showing(device, extra="NOISE"):
            rc, text = device.run_text("VCGrab", f"{remote} SECONDS 60 BITRATE 25000", 120)
        (artifacts / "report.txt").write_text(text)
        record_property("report", text.strip())
        assert rc != 0 and "took no more" in text, f"rc {rc}: {text}"
        n = numbers_of(text)
        assert n is not None and n["ms"] < 60000, f"it did not stop on the full disk: {text}"
        mkv = artifacts / "full.mkv"
        mkv.write_bytes(device.download(remote))
    assert mkv.stat().st_size > (FULL_DISK_MIB - 8) << 20, f"{mkv.stat().st_size} bytes of a {FULL_DISK_MIB} MiB volume"
    times = packet_times(mkv)
    record_property("pictures in the file", len(times))
    assert len(times) > 100, "what reached the disk does not play"


def test_a_screen_stepping_back_pauses(device, artifacts, record_property):
    """Behind the others for three seconds: no pictures of another screen, a gap, then the screen again, or a clean end if it moved."""
    job = device.start("VCTest", f"SCREEN WIDTH {W} HEIGHT {H} DEPTH 32 SECONDS 20 BACK 5")
    time.sleep(3)
    rc, text, n, mkv = grab(device, artifacts, "pause.mkv", "SECONDS 9")
    job.stop()
    assert n is not None, f"rc {rc}: {text}"
    record_property("report", text.strip())
    times = packet_times(mkv)
    gap = max(b - a for a, b in zip(times, times[1:]))
    record_property("largest gap, ms", gap)
    if "moved" in text:
        record_property("outcome", "the screen came back elsewhere: a clean end")
        assert n["paused"] > 0 and rc == 0
        return
    assert rc == 0 and n["paused"] >= 60 and gap >= 2000, f"gap {gap} ms: {text}"
    _, bars = pattern(W, H)
    assert not judge_bars(rgb_picture(mkv, len(times) - 2, W, H), bars), "the screen again at the end"


def test_stop_while_paused(device, artifacts, record_property):
    """Ctrl-C with the screen behind the others: a finished file, seekable."""
    screen = device.start("VCTest", f"SCREEN WIDTH {W} HEIGHT {H} DEPTH 32 SECONDS 20 BACK 5")
    time.sleep(3)
    job = device.start("VCGrab", "RAM:hw/stop.mkv")
    time.sleep(4)  # into the three seconds behind
    job.stop()
    text = job.wait(10)
    screen.stop()
    record_property("report", text.strip())
    n = numbers_of(text)
    assert n is not None and n["pictures"] > 0, text
    mkv = fetch(device, artifacts, "RAM:hw/stop.mkv", "stop.mkv")
    frames, _ = key_frames(mkv)
    assert frames > 0 and float(stream_fields(mkv)["duration"]) > 0, "a finished file"


def test_the_screen_closing_during_a_take(device, artifacts, record_property):
    """The screen closed under a running take: it counts as hidden, and the take ends as asked with a whole file."""
    screen = device.start("VCTest", f"SCREEN WIDTH {W} HEIGHT {H} DEPTH 32 SECONDS 6")
    time.sleep(3)
    rc, text, n, mkv = grab(device, artifacts, "closed.mkv", "SECONDS 8")
    screen.wait(10)
    record_property("report", text.strip())
    assert rc == 0 and n is not None and n["paused"] >= 60, f"rc {rc}: {text}"
    assert "no end mark" not in text and decode_errors(mkv) == "", "a whole file"


@pytest.mark.parametrize("args, words", [
    pytest.param("WIDTH 640 HEIGHT 256 NATIVE", "not an RTG screen", id="native"),
    pytest.param(f"WIDTH {W} HEIGHT {H} DEPTH 8", "no VideoCore equivalent", id="8-bit"),
])
def test_screens_it_cannot_take(device, record_property, args, words):
    """A native screen or an 8-bit one in front: a recording and a still both refused in plain words, no file made."""
    job = device.start("VCTest", f"SCREEN {args} SECONDS 20")
    time.sleep(3)
    if not device.procs("VCTest"):
        pytest.skip(f"no such screen here:\n{job.output()}")
    try:
        rc, text = device.run_text("VCGrab", "RAM:hw/refused.mkv SECONDS 2", 30)
        still_rc, still = device.run_text("VCGrab", "RAM:hw/refused.jpg STILL", 30)
    finally:
        job.stop()
    record_property("report", text.strip())
    assert rc == 10 and words in text, f"rc {rc}: {text}"
    assert still_rc == 10 and words in still, f"rc {still_rc}: {still}"
    for name in ("refused.mkv", "refused.jpg"):
        assert device.exec(f"List RAM:hw/{name}", 10)[0] != 0, f"{name} was made"


def beside_a_take(device, artifacts, record_property, n, text, mkv):
    """What every take beside other work must come to: finished, every picture decoding; what the other work cost it, recorded."""
    assert n is not None, text
    record_property("report", text.strip())
    record_property("dropped", f"{n['dropped']} of {n['pictures'] + n['dropped']}")
    assert "no end mark" not in text, text
    assert decode_errors(mkv) == "", "every picture decodes"
    assert n["dropped"] <= (n["pictures"] + n["dropped"]) // 4, f"the other work took the recording's place: {text}"


def test_beside_a_player_that_outlasts_it(device, material, artifacts, record_property):
    """A 1080p video playing in a window behind the screen, through the take and past its end."""
    player = device.start("VPUPlay", f"{material.ram('clip42.h264')} WINDOW LOOP")
    time.sleep(3)
    try:
        with showing(device):
            rc, text, n, mkv = grab(device, artifacts, "player.mkv", "SECONDS 10", 60)
        assert rc == 0, f"rc {rc}: {text}"
        assert device.procs("VPUPlay"), f"the player went with the take:\n{player.output()}"
    finally:
        player.stop()
    beside_a_take(device, artifacts, record_property, n, text, mkv)


def test_beside_a_player_that_ends_first(device, material, artifacts, record_property):
    """A video opened during the take and finished before it: the take runs on after the player's session closes."""
    with showing(device):
        take = device.start("VCGrab", "RAM:hw/player2.mkv SECONDS 14")
        time.sleep(2)
        r = device.run("VPUPlay", f"{material.ram('clip42.h264')} WINDOW FRAMES 150", 40)
        text = take.wait(40)
    assert r.passed, f"the player: {r.kv.get('fail')}"
    n = numbers_of(text)
    mkv = fetch(device, artifacts, "RAM:hw/player2.mkv", "player2.mkv")
    beside_a_take(device, artifacts, record_property, n, text, mkv)


def test_beside_a_decode_at_full_speed(device, material, artifacts, record_property):
    """The decoder flat out, no display: the worst a video can ask of the VPU, its pictures still bit exact."""
    clip = material.ram("clip42.h264")
    with showing(device):
        take = device.start("VCGrab", "RAM:hw/decode.mkv SECONDS 12")
        time.sleep(1)
        r = vctest(device, f"DECODE {clip} REF {material.ram('clip42.framemd5')}", 60)
        text = take.wait(40)
    assert r.passed and r.num("ref.mismatches") == 0, f"the decode: {r.kv.get('fail')}, {r.kv.get('ref.mismatches')} wrong"
    record_property("decode", f"{r.num('decode.frames')} frames")
    n = numbers_of(text)
    mkv = fetch(device, artifacts, "RAM:hw/decode.mkv", "decode.mkv")
    beside_a_take(device, artifacts, record_property, n, text, mkv)


def test_beside_sound(device, artifacts, record_property):
    """AHI playing through videocore.audio for the whole take: the tone neither late nor cut short."""
    with showing(device):
        tone = device.start("VCTest", f"AHITONE MODE {MODE_16BIT} MS 8000")
        time.sleep(1)
        rc, text, n, mkv = grab(device, artifacts, "sound.mkv", "SECONDS 5")
        r = tone.wait(30)
    assert rc == 0 and r.passed, f"rc {rc}: {text}; the tone: {r.kv.get('fail')}"
    beside_a_take(device, artifacts, record_property, n, text, mkv)


def test_beside_a_page_of_jpegs(device, material, artifacts, record_property):
    """Large JPEGs loading through vcjpeg.datatype, a process each, during the take: they queue, and all of them load."""
    with showing(device):
        take = device.start("VCGrab", "RAM:hw/page.mkv SECONDS 15")
        time.sleep(1)
        r = dt_page(device, material, ("j6000.jpg",), 4)
        text = take.wait(180)
    assert r.passed, f"the page: {r.kv.get('fail')}"
    n = numbers_of(text)
    mkv = fetch(device, artifacts, "RAM:hw/page.mkv", "page.mkv")
    beside_a_take(device, artifacts, record_property, n, text, mkv)


def test_stop_at_any_moment(device, artifacts, record_property):
    """Ctrl-C five times at no particular moment, pictures in flight or not: finished files every time."""
    with showing(device):
        for i in range(5):
            job = device.start("VCGrab", f"RAM:hw/any{i}.mkv")
            time.sleep(1.1 + i * 0.137)
            job.stop()
            text = job.wait(10)
            n = numbers_of(text)
            assert n is not None and n["pictures"] > 0 and "no end mark" not in text, text
            mkv = fetch(device, artifacts, f"RAM:hw/any{i}.mkv", f"any{i}.mkv")
            assert float(stream_fields(mkv)["duration"]) > 0.5
            assert decode_errors(mkv) == "", "every picture decodes"
            times = packet_times(mkv)
            assert len(times) == n["pictures"] and times == sorted(set(times)), f"{text}: the file's times are not the report's, or repeat: {times[-4:]}"


@pytest.mark.smoke
def test_still(device, artifacts, record_property):
    """One JPEG of the screen: the bars right."""
    with showing(device):
        rc, text = device.run_text("VCGrab", "RAM:hw/still.jpg STILL", 30)
    assert rc == 0, text
    record_property("report", text.strip())
    jpg = fetch(device, artifacts, "RAM:hw/still.jpg", "still.jpg")
    data = ffmpeg("-i", str(jpg), "-f", "rawvideo", "-pix_fmt", "rgb24", "-")
    _, bars = pattern(W, H)
    problems = judge_bars(np.frombuffer(data, np.uint8).reshape(H, W, 3), bars)
    assert not problems, problems


def test_commodity(device, artifacts, record_property):
    """The hot keys: start, a still in the middle, stop, again; numbered files, Ctrl-C ending it with a recording under way."""
    with showing(device):
        job = device.start("VCGrab", "RAM:hw/take CX")
        try:
            time.sleep(1)
            hot_key(device, KEY_R)
            time.sleep(3)
            hot_key(device, KEY_G)
            time.sleep(2)
            hot_key(device, KEY_S)
            time.sleep(2.5)
            hot_key(device, KEY_R)
            time.sleep(2)
        finally:
            job.stop()  # it finishes its file
        text = job.wait(15)
    (artifacts / "commodity.txt").write_text(text)
    record_property("report", text.strip())
    reports = [numbers_of(line) for line in text.splitlines() if numbers_of(line)]
    assert len(reports) == 2, text
    record_property("ticks the still cost", reports[0]["dropped"])
    assert reports[0]["dropped"] <= 12, text
    first = fetch(device, artifacts, "RAM:hw/take-0001.mkv", "take-0001.mkv")
    still = fetch(device, artifacts, "RAM:hw/take-0002.jpg", "take-0002.jpg")
    second = fetch(device, artifacts, "RAM:hw/take-0003.mkv", "take-0003.mkv")
    assert key_frames(first)[0] >= 120 and key_frames(second)[0] >= 30, text
    assert decode_errors(still) == ""


@contextmanager
def commodity(device, args="CX", saved=None):
    """VCGrab in the background, with @saved in its settings file, and the bars in front for it."""
    if saved is not None:
        device.upload_bytes(PREFS, saved.encode())
    with showing(device):
        job = device.start("VCGrab", args)
        time.sleep(1)
        try:
            yield job
        finally:
            job.stop()


def a_take(device, start=KEY_R, stop=KEY_S, seconds=3):
    """A recording by the hot keys."""
    hot_key(device, start)
    time.sleep(seconds)
    hot_key(device, stop)
    time.sleep(2)


def test_saved_settings(device, artifacts, record_property):
    """What the settings file says is what the commodity does: the name, the size, the rate, the bit rate and the keys."""
    saved = "TO = RAM:hw/saved\nSIZE = 1280x720\nFPS = 25\nBITRATE = 2000\nSTARTKEY = ctrl alt f\nSTOPKEY = ctrl alt h\n"
    with commodity(device, saved=saved):
        a_take(device, KEY_F, KEY_H)
    mkv = fetch(device, artifacts, "RAM:hw/saved-0001.mkv", "saved.mkv")
    fields = stream_fields(mkv)
    assert (fields["width"], fields["height"]) == ("1280", "720"), fields
    times = packet_times(mkv)
    record_property("picture times", times[:4])
    assert times[1] - times[0] == 40, f"not 25 pictures a second: {times[:4]}"


def test_arguments_beat_the_saved_settings(device, artifacts):
    """An argument holds for the run, and the file stays as it was."""
    saved = "TO = RAM:hw/saved\nFPS = 25\n"
    with commodity(device, "RAM:hw/given CX FPS 30", saved):
        a_take(device)
    mkv = fetch(device, artifacts, "RAM:hw/given-0001.mkv", "given.mkv")
    times = packet_times(mkv)
    assert times[1] - times[0] in (33, 34), f"not 30 pictures a second: {times[:4]}"
    assert device.download(PREFS).decode() == saved


def test_one_shot_ignores_the_saved_settings(device, artifacts):
    """A script gets what it asks for, whatever the commodity was told."""
    device.upload_bytes(PREFS, b"SIZE = 640x360\nFPS = 25\n")
    with showing(device):
        rc, text, n, mkv = grab(device, artifacts, "plain.mkv", "SECONDS 2")
    assert rc == 0 and n is not None and n["width"] == W, text


def test_the_file_is_named(device):
    """Only the commodity makes up names."""
    rc, text = device.run_text("VCGrab", "SECONDS 1", 30)
    assert rc == 20 and "TO" in text, f"rc {rc}: {text}"


def test_commodity_needs_no_name(device, artifacts):
    """Nothing saved and nothing given: takes go to RAM:grab."""
    with commodity(device):
        a_take(device)
    assert stream_fields(fetch(device, artifacts, "RAM:grab-0001.mkv", "default.mkv"))["width"] == str(W)


def test_a_key_turned_off(device, artifacts):
    """NONE means no such key: the take ends by its time limit, not by the Stop key that was the default."""
    with commodity(device, saved="TO = RAM:hw/limit\nSTOPKEY = NONE\nSECONDS = 2\n"):
        hot_key(device, KEY_R)
        time.sleep(1)
        hot_key(device, KEY_S)
        time.sleep(4)
    assert float(stream_fields(fetch(device, artifacts, "RAM:hw/limit-0001.mkv", "limit.mkv"))["duration"]) > 1.8


def test_a_bad_saved_key(device, artifacts):
    """A key that describes nothing costs the file's keys, not the commodity."""
    with commodity(device, saved="TO = RAM:hw/fell\nSTARTKEY = no such key\n"):
        a_take(device)
    assert stream_fields(fetch(device, artifacts, "RAM:hw/fell-0001.mkv", "fell.mkv"))["width"] == str(W)


def test_too_long_in_the_file(device, artifacts):
    """A saved value that does not fit leaves what was there, not the first of it."""
    with commodity(device, saved=f"TO = RAM:hw/kept\nTO = RAM:hw/{'a' * 300}\n"):
        a_take(device)
    assert stream_fields(fetch(device, artifacts, "RAM:hw/kept-0001.mkv", "kept.mkv"))["width"] == str(W)


def test_the_window_shows_and_hides(device):
    """The popup key shows the settings window, its close gadget hides it, and the commodity goes on."""
    job = device.start("VCGrab", "RAM:hw/win CX")
    try:
        time.sleep(2)
        assert '"VCGrab"' not in device.ami.ui_tree(), "a shell start stays out of sight"
        hot_key(device, KEY_V)
        time.sleep(2)
        device.window("VCGrab")
        device.ami.ui_click("close", "VCGrab")
        time.sleep(1)
        assert '"VCGrab"' not in device.ami.ui_tree(), "still shown"
        assert device.procs("VCGrab"), "the commodity went with the window"
    finally:
        job.stop()


def test_a_second_start_shows_the_window(device):
    """Starting it again shows the first one's window and ends, rc 0."""
    job = device.start("VCGrab", "RAM:hw/win CX")
    try:
        time.sleep(2)
        rc, text = device.run_text("VCGrab", "RAM:hw/other CX", 30)
        assert rc == 0, f"rc {rc}: {text}"
        time.sleep(2)
        device.window("VCGrab")
    finally:
        job.stop()


def test_plays_back_through_the_vpu(device, artifacts, record_property):
    """What was recorded, on the VideoCore's own decoder and display path: the bars right on screen, so full range survives it."""
    with showing(device):
        rc, text, n, mkv = grab(device, artifacts, "back.mkv", "SECONDS 3")
    assert rc == 0 and n is not None, f"rc {rc}: {text}"
    h264 = artifacts / "back.h264"
    ffmpeg("-i", str(mkv), "-c", "copy", "-bsf:v", "h264_mp4toannexb", "-f", "h264", str(h264))
    device.upload_bytes("RAM:hw/back.h264", h264.read_bytes())
    job = device.start("VPUPlay", "RAM:hw/back.h264 LOOP DEPTH 32")
    time.sleep(5)
    _, bars = pattern(W, H)
    problems = judge_screen_bars(device, bars, "back")
    job.stop()
    device.delete("RAM:hw/back.h264")
    record_property("problems", problems)
    assert not problems, problems


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left, no stray message."""
    assert_nothing_left(device)
