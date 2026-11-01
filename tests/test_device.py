# SPDX-License-Identifier: BSD-3-Clause
"""vchiq.device: the connection, a bulk transfer, the codec table as the library reports it, expunge and resume."""
import pytest

from harness.tools import assert_nothing_left, status, vctest

STILL_FORMATS = "I420,RGB16,RGB24,BGR24,BGRA32,RGBA32"


@pytest.mark.smoke
def test_connection_up(device, record_property):
    """VCTest STATUS: the device opens, its connection busy and connected, how it came up recorded."""
    r = status(device)
    assert r.num("vchiq.flags") & 0x9 == 0x9, f"flags {r.kv['vchiq.flags']}: not busy and connected"
    record_property("bringup", r.kv.get("vchiq.bringup", "?"))
    record_property("services open", r.kv.get("vchiq.services"))
    assert r.num("backends") >= 1, "no codec backend"


@pytest.mark.smoke
def test_codec_table(device):
    """VCTest CODECS: eleven rows; the streams on sessions, the stills on image calls, every still decoder in six formats."""
    r = vctest(device, "CODECS", 30)
    assert r.passed, r.kv.get("fail")
    rows = r.group("codec")
    assert len(rows) == 11, f"{len(rows)} codec rows"
    by_name = {(c["name"], c["kind"]): c for c in rows}
    for name in ("H264", "MJPEG"):
        assert by_name[(name, "decoder")]["session"] == "yes", f"{name} decoder: no sessions"
    assert by_name[("H264", "encoder")]["session"] == "yes"
    assert int(by_name[("H264", "decoder")]["level"]) == 42, "the H.264 decoder's level is not 4.2"
    assert int(by_name[("H264", "encoder")]["level"]) == 40, "the H.264 encoder's level is not 4.0"
    for name in ("JPEG", "PNG", "GIF", "BMP", "TGA", "PPM"):
        row = by_name[(name, "decoder")]
        assert row["image"] == "yes", f"{name} decoder: no image calls"
        assert row["formats"] == STILL_FORMATS, f"{name} decoder writes {row['formats']}"
    assert by_name[("JPEG", "encoder")]["image"] == "yes"
    assert by_name[("raw", "converter")]["image"] == "yes"


@pytest.mark.smoke
def test_audio_bulk(device, record_property):
    """VCTest AUDIO MS 1000: ten chunks of silence by bulk transmit to the audio service, all consumed."""
    r = vctest(device, "AUDIO MS 1000", 60)
    assert r.passed, r.kv.get("fail")
    assert r.num("run.0.chunks") == 10 and r.num("run.0.chunk_bytes") == 19200, "not ten chunks of 19200 bytes"
    assert r.num("run.0.completed") == 192000, f"{r.kv['run.0.completed']} bytes completed"
    record_property("COMPLETEs flagged", r.kv.get("run.0.flagged"))


def test_resume_after_expunge(device, driver_log):
    """Avail FLUSH expunges the device (it quiesces); its next open resumes the session."""
    device.flush()
    mark = driver_log.mark()
    r = status(device)
    assert r.kv.get("vchiq.bringup") == "resumed", f"bringup {r.kv.get('vchiq.bringup')}"
    driver_log.since(mark).expect(r"connection live, quiesced", "the device's log")


@pytest.mark.smoke
def test_nothing_left(device):
    """No session left, no stray message."""
    assert_nothing_left(device)
