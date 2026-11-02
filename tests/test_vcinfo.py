# SPDX-License-Identifier: BSD-3-Clause
"""VCInfo, the user's diagnostic, as a product: its prose and return codes."""
import re

import pytest

from harness.results import expect

CODEC_ROWS = (r"^11 codecs$", r"level 4\.0", r"H264 decoder: .*, session, priority", r"MJPEG decoder: .*, session, priority",
              r"H264 encoder: .*; reads I420 RGB16 RGB24 BGR24 BGRA32 RGBA32; [^,]+, session, priority",
              r"JPEG encoder: .*; reads I420 RGBA32; [^,]+, session, image call,",
              r"raw converter: .*; reads I420 NV12 RGB16 RGB24 BGR24 BGRA32 RGBA32 YUYV; [^,]+, session, image call,",
              *(rf"{c} decoder: .*writes I420 RGB16 RGB24 BGR24 BGRA32 RGBA32; [^,]+, image call,"
                for c in ("JPEG", "PNG", "GIF", "BMP", "TGA", "PPM")))


@pytest.mark.smoke
def test_summary(device, record_property):
    """VCInfo alone: the setup in plain words, every part there, PASS (warnings allowed: they are the machine's)."""
    rc, text = device.run_text("VCInfo", "")
    assert rc in (0, 5), f"return code {rc}:\n{text}"
    for regex in (r"^  VCHIQ node: ", r"^  vchiq\.device: .*connected", r"^  firmware: ", r"^  temperature: ", r"^  gpu_mem: ",
                  r"codec memory \d+ MiB free", r"^PASS"):
        expect(text, regex, "the summary")
    for what in ("throttling", "gpu_mem", "vchiq\\.device"):
        m = re.search(rf"^  {what}: .*$", text, re.M)
        record_property(what, m.group(0).strip() if m else "no line")


def test_window_dump(device):
    """VCInfo DUMP: the shared window, read only."""
    rc, text = device.run_text("VCInfo", "DUMP")
    expect(text, r"^PASS$", "DUMP")
    expect(text, r"host: flags 0x", "the host side of the window")
    expect(text, r"VPU debug\[\]", "the VPU side of the window")


def test_gencmd(device):
    """VCInfo GCMD with a command of two words: gencmd's answer, status 0."""
    rc, text = device.run_text("VCInfo", 'GCMD "measure_clock arm"')
    expect(text, r"^PASS$", "GCMD")
    expect(text, r'"measure_clock arm" -> status 0, .*: frequency\(\d+\)=\d+$', "gencmd's answer")


def test_components(device, artifacts, record_property):
    """VCInfo COMPONENTS: the firmware's MMAL components and codec switches, kept whole in the artifacts."""
    rc, text = device.run_text("VCInfo", "COMPONENTS", timeout=120)
    expect(text, r"^PASS$", "COMPONENTS")
    (artifacts / "components.txt").write_text(text, encoding="latin-1")
    for comp in ("ril.image_decode", "ril.video_encode", "ril.audio_decode"):
        m = re.search(rf"^{re.escape(comp)}: .*$", text, re.M)
        record_property(comp, m.group(0) if m else "not listed")
    record_property("MJPG on ril.video_encode", "yes" if re.search(r"MJPG", text) else "no")


def test_codec_table(device):
    """VCInfo CODECS: eleven rows, each saying what serves it: sessions for the streams, image calls for the stills."""
    rc, text = device.run_text("VCInfo", "CODECS")
    expect(text, r"^PASS$", "CODECS")
    for regex in CODEC_ROWS:
        expect(text, regex, "the codec rows")
