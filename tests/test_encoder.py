# SPDX-License-Identifier: BSD-3-Clause
"""The H.264 encoder: thirty pictures with key frames, a key frame on request, and RGB through the ISP inside the VPU."""
import numpy as np
import pytest

from harness.material import ffmpeg
from harness.pictures import judge_bars, key_frames, pattern, psnr, raw_bars, y_planes
from harness.tools import assert_nothing_left, derived, vctest
from test_vcgrab import rgb_picture, stream_fields

W, H = 640, 360     # the bars through the ISP
PICTURES = 12
PACKET = 512 * 1024 # a key frame of noise is over 1 MB
NOISE_BITS = 25000000


def encode(device, material, artifacts, extra, name):
    """VCTest ENCODE of the thirty raw pictures; the stream fetched into the artifacts: (result, local path)."""
    clip = derived(device, material, "clip.i420")
    r = vctest(device, f"ENCODE {clip} WIDTH 1920 HEIGHT 1080 FPS 30 {extra} OUT RAM:hw/{name}", 120)
    assert r.passed, r.kv.get("fail")
    local = artifacts / name
    local.write_bytes(device.download(f"RAM:hw/{name}"))
    device.delete(f"RAM:hw/{name}")
    return r, local


@pytest.mark.smoke
def test_h264_encode(device, material, baselines, artifacts, record_property):
    """Thirty pictures at 4 Mbit/s, a key frame every ten: decodable, three key frames, close to the source."""
    r, h264 = encode(device, material, artifacts, "BITRATE 4000000 KEYINT 10", "out.h264")
    frames, keys = key_frames(h264)
    record_property("key frames", keys)
    assert frames == 30 and len(keys) == 3, f"{frames} frames, key frames at {keys}"
    got = y_planes(ffmpeg("-i", str(h264), "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"), 1920, 1080, 30)
    ref = y_planes(material.path("clip30.i420"), 1920, 1080, 30)
    score = sum(psnr(a, b) for a, b in zip(got, ref)) / len(got)
    baselines.check("h264_encode_4mbit.psnr_db", round(score, 1), higher=True, tolerance=2.0)
    baselines.check("h264_encode_4mbit.encode.fps", r.num("encode.fps"), higher=True, tolerance="15%")


def test_key_frame_on_request(device, material, artifacts, record_property):
    """VCA_RequestKeyFrame before picture 15, with key frames otherwise every hundred."""
    r, h264 = encode(device, material, artifacts, "KEYINT 100 KEYREQ 15", "key.h264")
    assert r.num("keyreq.rc") == 0, f"the request was refused: {r.kv.get('keyreq.rc')}"
    frames, keys = key_frames(h264)
    record_property("key frames", keys)
    assert 0 in keys and any(k in keys for k in (14, 15, 16)), f"key frames at {keys}"


def bars(device, fmt):
    """Twelve pictures of the bars in @fmt on the Amiga, once per boot."""
    remote = f"RAM:hw/enc-bars.{fmt}"
    if remote not in device.ram_known:
        device.upload_bytes(remote, raw_bars(fmt, W, H) * PICTURES)
        device.ram_known.add(remote)
    return remote


def through_isp(device, artifacts, name, extra="", fmt="BGRA32", bitrate=4000000):
    """The bars through an encoder session in @fmt: (result, the stream fetched)."""
    r = vctest(device, f"ENCODE {bars(device, fmt)} WIDTH {W} HEIGHT {H} FORMAT {fmt} FPS 30 BITRATE {bitrate} KEYINT 5 {extra} OUT RAM:hw/{name}", 60)
    local = artifacts / name
    if r.passed:
        local.write_bytes(device.download(f"RAM:hw/{name}"))
    device.delete(f"RAM:hw/{name}")
    return r, local


@pytest.mark.smoke
@pytest.mark.parametrize("fmt", ["RGB16", "RGB24", "BGR24", "BGRA32", "RGBA32"])
def test_rgb_through_the_isp(device, artifacts, record_property, fmt):
    """Each format in as it is: every picture out, its time kept, the colours right, full range in the stream."""
    r, h264 = through_isp(device, artifacts, f"isp-{fmt}.h264", fmt=fmt)
    assert r.passed, r.kv.get("fail")
    assert r.num("encode.pictures_out") == PICTURES and r.num("encode.pts_wrong") == 0, r.kv
    fields = stream_fields(h264)
    assert fields["color_range"] == "pc" and fields["color_space"] == "smpte170m", fields
    problems = judge_bars(rgb_picture(h264, PICTURES - 1, W, H), pattern(W, H)[1])
    record_property("bars", problems or "right")
    assert not problems, problems


@pytest.mark.parametrize("size", [(320, 180), (1280, 720)], ids=["down", "up"])
def test_scaled_through_the_isp(device, artifacts, size):
    """VCA_OutputWidth and VCA_OutputHeight are the size encoded, the bars where they belong."""
    width, height = size
    r, h264 = through_isp(device, artifacts, "isp-scaled.h264", f"OUTWIDTH {width} OUTHEIGHT {height}")
    assert r.passed, r.kv.get("fail")
    fields = stream_fields(h264)
    assert (int(fields["width"]), int(fields["height"])) == size, fields
    problems = judge_bars(rgb_picture(h264, 0, width, height), pattern(W, H)[1], scale=width / W, tolerance=12)
    assert not problems, problems


def test_gaps_in_the_times_go_through(device, artifacts):
    """A recorder's dropped ticks are gaps in the times: the ISP leaves them as they are."""
    r, _ = through_isp(device, artifacts, "isp-gaps.h264", "GAPS")
    assert r.passed and r.num("encode.pts_wrong") == 0, r.kv


def test_the_end_mark_alone(device, artifacts):
    """VCFF_NoPicture: the stream ends with no picture read for it, all the others there."""
    r, h264 = through_isp(device, artifacts, "isp-empty.h264", "ENDEMPTY")
    assert r.passed and r.kv.get("encode.end") == "yes", r.kv
    assert key_frames(h264)[0] == PICTURES


def test_a_refused_bit_rate_unwinds(device, artifacts):
    """Past level 4.0 the first send fails inside the tunnel, and the VPU takes the next session."""
    r, _ = through_isp(device, artifacts, "isp-refused.h264", bitrate=30000000)
    assert not r.passed, "30 Mbit/s was taken"
    again, _ = through_isp(device, artifacts, "isp-again.h264")
    assert again.passed, again.kv.get("fail")


def test_refuses_unlisted_input(device, material):
    """An input format the H.264 encoder does not name (a camera's YUYV) is refused at the open, VCERR_UNSUPPORTED."""
    r = vctest(device, f"ENCODE {material.ram('frame640.yuyv')} WIDTH 640 HEIGHT 480 FORMAT YUYV OUT RAM:hw/yuyv.h264")
    assert not r.passed and r.kv.get("encode.session") == "refused", f"session {r.kv.get('encode.session')}: {r.kv.get('fail')}"
    assert r.num("error") == -6, f"error {r.kv.get('error')}, not VCERR_UNSUPPORTED"


def test_closing_with_the_tunnel_stalled(device, artifacts, record_property):
    """Packets held back, the encoder stopped behind them: the close takes no time and the service lives on."""
    worst = 0
    for _ in range(5):
        r, _ = through_isp(device, artifacts, "isp-held.h264", "HOLD 2")
        assert r.passed and r.num("encode.held") > 0, r.kv
        worst = max(worst, r.num("encode.close_ms"))
    record_property("worst close, ms", worst)
    assert worst < 1000
    again, _ = through_isp(device, artifacts, "isp-again.h264")
    assert again.passed, again.kv.get("fail")


@pytest.mark.slow
def test_noise_through_the_isp(device, artifacts, record_property):
    """1080p noise at a recorder's pace: the encoder falls behind and frames come back late, no picture is lost."""
    noise = np.random.default_rng(7).integers(0, 256, (1080, 1920, 2), dtype=np.uint8)
    names = []
    for i in range(6):
        names.append(f"RAM:hw/noise{i}")
        device.upload_bytes(names[-1], np.roll(noise, i * 997, axis=1).tobytes())
    assert device.exec("C:Join " + " ".join(names) + " AS RAM:hw/six", timeout=60)[0] == 0
    assert device.exec("C:Join " + " ".join(["RAM:hw/six"] * 10) + " AS RAM:hw/noise.raw", timeout=60)[0] == 0
    r = vctest(device, f"ENCODE RAM:hw/noise.raw WIDTH 1920 HEIGHT 1080 FORMAT RGB16 FPS 30 BITRATE {NOISE_BITS} KEYINT 60 PACE PACKET {PACKET} OUT RAM:hw/noise.h264", 90)
    for name in names + ["RAM:hw/six", "RAM:hw/noise.raw", "RAM:hw/noise.h264"]:
        device.delete(name)
    assert r.passed, r.kv.get("fail")
    record_property("late", r.num("encode.late"))
    assert r.num("encode.pictures_out") == 60 and r.num("encode.pts_wrong") == 0, r.kv


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left, no stray message."""
    assert_nothing_left(device)
