# SPDX-License-Identifier: BSD-3-Clause
"""The H.264 and MJPEG decoder through VCTest DECODE: bit-exact decodes, refusals, speed, two clients, flush, shapes, a chain."""
import numpy as np
import pytest

from harness.material import ffmpeg
from harness.pictures import psnr, y_planes
from harness.tools import assert_nothing_left, vctest

CHAINED_LEAST_DB = 40.0  # the chain's luma against ffmpeg's: the converter only repacks it, a wrong picture scores 10 to 15


def decode(device, material, clip, extra="", ref=True, timeout=180):
    """VCTest DECODE of a stream of the material, against its framemd5 unless @ref is False."""
    args = f"DECODE {material.ram(clip)}"
    if ref:
        args += f" REF {material.ram(clip.replace('.h264', '.framemd5'))}"
    return vctest(device, f"{args} {extra}", timeout)


@pytest.mark.smoke
def test_bit_exact(device, material):
    """Three hundred frames of 1080p, every one the same as ffmpeg's."""
    r = decode(device, material, "clip42.h264")
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.frames") == 300 and r.num("decode.width") == 1920
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


@pytest.mark.smoke
def test_bit_exact_framed(device, material):
    """One access unit per packet with timestamps (FRAMED): the same three hundred frames bit exact."""
    r = decode(device, material, "clip42.h264", "FRAMED")
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


def test_extradata(device, material):
    """The SPS and PPS passed as VCA_Extradata instead of in the stream: the same three hundred frames bit exact."""
    r = decode(device, material, "clip42.h264", "FRAMED EXTRADATA")
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.extradata") > 0
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


def test_extradata_large(device, material):
    """A codec configuration over 128 bytes (PAD 200: zeros after the PPS): it goes whole, the frames bit exact."""
    r = decode(device, material, "clip42.h264", "FRAMED EXTRADATA PAD 200")
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.extradata") > 200
    assert r.num("ref.compared") == 300 and r.num("ref.mismatches") == 0


def test_backend_named(device, material):
    """The backend asked for by name (VCA_Backend videocore): the same result, on the first 150 frames."""
    r = decode(device, material, "clip42.h264", "FRAMED FRAMES 150 BACKEND videocore")
    assert r.passed, r.kv.get("fail")
    assert r.num("ref.compared") == 150 and r.num("ref.mismatches") == 0


def test_codec_tag(device, material):
    """VCA_CodecTag avc1, the FourCC of an MP4's H.264: videocore.codec's rows serve every form, so the same frames."""
    r = decode(device, material, "clip42.h264", "FRAMED FRAMES 150 TAG avc1")
    assert r.passed, r.kv.get("fail")
    assert r.kv.get("decode.tag") == "avc1"
    assert r.num("ref.compared") == 150 and r.num("ref.mismatches") == 0


@pytest.mark.parametrize("extra", [pytest.param("", id="native"), pytest.param("OUTFORMAT RGBA32", id="through-isp")])
def test_resignal(device, material, extra):
    """The session moved to another signal bit after thirty frames (VC_SetAttrsTags(VCA_SigMask)), VCTest waiting for that
    bit only from then on: every frame still comes, so each of the backend's components signals the new bit."""
    r = decode(device, material, "clip42.h264", f"RESIGNAL 30 {extra}", ref=extra == "")
    assert r.passed, r.kv.get("fail")
    assert r.num("resignal.at") == 30 and r.num("resignal.rc") == 0
    assert r.num("decode.frames") == 300


def test_unknown_backend_refused(device, material):
    """A backend that does not exist is refused at once (-3), no hang."""
    r = decode(device, material, "clip42.h264", "BACKEND nosuch", ref=False, timeout=30)
    assert r.rc == 20 and r.kv.get("decode.session") == "refused"
    assert r.num("error") == -3, f"error {r.kv.get('error')}"


def test_decoder_rate(device, material, baselines):
    """The decoder's own rate, no hashing: the baseline of the hardware."""
    r = decode(device, material, "clip42.h264", "FRAMES 300 NOHASH", ref=False, timeout=120)
    assert r.passed, r.kv.get("fail")
    baselines.check("decoder_alone.decode.fps", r.num("decode.fps"), higher=True, tolerance="10%")


def test_two_decodes_at_once(device, material):
    """Two decoder sessions on one service, each its own stream (a frame crossing over would show), both bit exact."""
    args = [f"DECODE {material.ram(f'{n}.h264')} REF {material.ram(f'{n}.framemd5')}" for n in ("clip42", "l-base")]
    a = device.start("VCTest", args[0])
    b = device.start("VCTest", f"{args[1]} FRAMED")
    ra, rb = a.wait(180), b.wait(180)
    for r in (ra, rb):
        assert r.passed, r.kv.get("fail")
        assert r.num("ref.mismatches") == 0


def test_mjpeg_one_picture(device, material):
    """A JPEG as one MJPEG packet through the video decoder: the native I420 picture, the end mark with it."""
    r = vctest(device, f"DECODE {material.ram('pic.jpg')}", 60)
    assert r.passed, r.kv.get("fail")
    assert r.num("decode.frames") == 1 and r.num("decode.width") == 1920


@pytest.mark.parametrize("extra", [pytest.param("", id="native"), pytest.param("OUTFORMAT RGBA32", id="through-isp")])
def test_flush_replays(device, material, extra):
    """VC_Flush() after a hundred frames, the clip again: no frame from before the flush comes after it."""
    r = decode(device, material, "clip42.h264", f"FRAMED NOHASH FLUSH 100 {extra}", ref=False, timeout=120)
    assert r.passed, r.kv.get("fail")
    assert r.num("flush.rc") == 0 and r.num("flush.frames") > 0
    assert r.num("flush.stale") == 0 and r.num("flush.untimed") == 0


def test_chained_output(device, material, record_property):
    """YUYV, which the H.264 decoder does not write itself: the library puts the VideoCore's converter behind it (the
    chain, as VCCodecInfo.ChainOutputFormats lists it), and the first picture's luma is ffmpeg's."""
    out = "RAM:hw/chained.yuyv"
    r = decode(device, material, "clip42.h264", f"OUTFORMAT YUYV FRAMES 1 OUT {out}", ref=False, timeout=60)
    try:
        assert r.passed, r.kv.get("fail")
        data = device.download(out)
    finally:
        device.delete(out)
    w, h = int(r.num("frame.first.width")), int(r.num("frame.first.height"))
    luma = np.frombuffer(data, np.uint8, w * h * 2).reshape(h, w * 2)[:, 0::2]
    ref = ffmpeg("-i", str(material.path("clip42.h264")), "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-")
    score = psnr(luma, y_planes(ref, w, h, 1)[0])
    record_property("psnr", f"{score:.1f} dB")
    assert score >= CHAINED_LEAST_DB, f"{score:.1f} dB: the chained picture is not the stream's"


def test_tunnel_start(device, material):
    """Six starts through the ISP: each gives its first picture at once and every frame (a bad start order loses about every second one)."""
    for _ in range(6):
        r = decode(device, material, "clip42.h264", "FRAMED NOHASH OUTFORMAT BGRA32", ref=False, timeout=60)
        assert r.passed, r.kv.get("fail")
        assert r.num("decode.frames") == 300
        assert r.num("decode.first_frame_after") < 60


def test_shape_change_through_isp(device, material):
    """A stream that changes from 1080p to 720p, through the ISP: the frames keep the first shape, the picture scaled."""
    r = vctest(device, f"DECODE {material.ram('two.h264')} OUTFORMAT RGBA32 VERBOSE", 120)
    assert r.passed, r.kv.get("fail")
    streams = r.group("stream")
    assert [s["width"] for s in streams] == ["1920"], f"shapes reported: {[s['width'] for s in streams]}"
    assert r.num("decode.frames") == 120


def test_shape_change_native(device, material):
    """The same stream natively: both shapes reported, bit exact on both sides of the change."""
    r = vctest(device, f"DECODE {material.ram('two.h264')} REF {material.ram('two.framemd5')} VERBOSE", 120)
    assert r.passed, r.kv.get("fail")
    assert [s["width"] for s in r.group("stream")] == ["1920", "1280"]
    assert r.num("ref.compared") == 120 and r.num("ref.mismatches") == 0


@pytest.mark.parametrize("clip,par", [("wide.h264", (64, 45)), ("wide-idc.h264", (16, 11))])
def test_pixel_aspect_reported(device, material, clip, par):
    """A 720x576 stream of wide pixels: the pixel aspect from the SPS, an extended one and a predefined one."""
    r = vctest(device, f"DECODE {material.ram(clip)} NOHASH FRAMES 2", 60)
    assert r.passed, r.kv.get("fail")
    assert (r.num("stream.0.par_num"), r.num("stream.0.par_den")) == par


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left, no stray message."""
    assert_nothing_left(device)
