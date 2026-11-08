# SPDX-License-Identifier: BSD-3-Clause
"""How videocodec.library chooses a backend by the pictures asked for (architecture.md 5.2): with a second converter
installed, the software one of tests/swconv/ at software priority, a format only it writes goes to it, one both write
to the VideoCore's, and a program may still name it.  The backend is put in LIBS:videocodec/ for this module and taken
out again, each with a reboot: the library reads the drawer once."""
import time
from pathlib import Path

import numpy as np
import pytest

from harness.pictures import best_psnr, ref_rgb
from harness.tools import backend, status, vctest

SWCONV = "LIBS:videocodec/swconv.codec"
W, H = 800, 600
LEAST_DB = 30.0
PFS3_SETTLE = 20  # PFS3 writes late: a reboot right after a copy can lose it

pytestmark = pytest.mark.reboot


@pytest.fixture(scope="module")
def swconv(request, device):
    """swconv.codec in LIBS:videocodec/ from the build tree, the library made to read the drawer again; out at the end."""
    local = Path(request.config.getoption("--build-dir"), "emu68-vchiq-library", "tests", "swconv", "swconv.codec")
    if not local.is_file():
        pytest.skip(f"no {local}: build the emu68-vchiq-library target")
    device.upload(local, SWCONV)
    time.sleep(PFS3_SETTLE)
    device.reboot()
    yield
    device.delete(SWCONV)
    time.sleep(PFS3_SETTLE)
    device.reboot()


def conversions(device) -> int:
    """How many image calls swconv.codec has converted."""
    return int(backend(status(device), "swconv.codec")["counter.conversions"])


def convert(device, material, fmt, extra=""):
    """VCTest CONVERT of frame800.i420 into @fmt at its own size."""
    return vctest(device, f"CONVERT {material.ram('frame800.i420')} WIDTH {W} HEIGHT {H} OUTFORMAT {fmt} {extra} "
                          "OUT RAM:hw/choice.raw", 60)


def check_picture(device, material, fmt, artifacts):
    """The converted picture against ffmpeg's reading of the source; its alpha 255."""
    data = device.download("RAM:hw/choice.raw")
    (artifacts / f"{fmt}.raw").write_bytes(data)
    quads = np.frombuffer(data, np.uint8).reshape(H, W, 4)
    rgb, alpha = (quads[:, :, 1:4], quads[:, :, 0]) if fmt == "ARGB32" else (quads[:, :, 0:3], quads[:, :, 3])
    src = ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", f"{W}x{H}", "-i", str(material.path("frame800.i420"))]
    score, how = best_psnr(rgb, ref_rgb(src, W, H))
    assert score >= LEAST_DB, f"{score:.1f} dB ({how})"
    assert int(alpha.min()) == 255
    return score, how


def test_listed(device, swconv):
    """Both converters listed: swconv.codec with its one row, nothing converted yet."""
    assert conversions(device) == 0


def test_only_software_writes(device, material, artifacts, record_property, swconv):
    """ARGB32, which the VideoCore does not write: the software converter takes the call, and converts it right."""
    before = conversions(device)
    r = convert(device, material, "ARGB32")
    assert r.passed, f"VC_ConvertImage {r.kv.get('convert.rc')}: {r.kv.get('fail')}"
    assert conversions(device) == before + 1, "swconv.codec did not convert it"
    score, how = check_picture(device, material, "ARGB32", artifacts)
    record_property("psnr", f"{score:.1f} dB as {how}")


def test_hardware_wins(device, material, swconv):
    """RGBA32, which both write: the VideoCore's row, of the higher priority, takes the call."""
    before = conversions(device)
    r = convert(device, material, "RGBA32")
    assert r.passed, f"VC_ConvertImage {r.kv.get('convert.rc')}: {r.kv.get('fail')}"
    assert conversions(device) == before, "swconv.codec converted what the VideoCore writes"


def test_named(device, material, artifacts, swconv):
    """VCA_Backend swconv: the program's choice stands, RGBA32 on the 68k."""
    before = conversions(device)
    r = convert(device, material, "RGBA32", "BACKEND swconv")
    assert r.passed, f"VC_ConvertImage {r.kv.get('convert.rc')}: {r.kv.get('fail')}"
    assert conversions(device) == before + 1
    check_picture(device, material, "RGBA32", artifacts)


def test_none_takes_it(device, material, swconv):
    """RGB15, which neither writes: the best row all the same, whose refusal says why (VCERR_UNSUPPORTED)."""
    r = convert(device, material, "RGB15")
    assert not r.passed and r.kv.get("convert.rc") == "-6", f"rc {r.rc}, convert.rc {r.kv.get('convert.rc')}"
