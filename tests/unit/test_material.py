# SPDX-License-Identifier: BSD-3-Clause
"""The material recipes (harness/material.py): the cache naming, the sample's check and the pattern recipes only."""
import shutil
from pathlib import Path

import numpy as np
import pytest

from harness import material
from harness.material import MATERIAL_VERSION, MaterialError, Material, RECIPES, cache_dir, ensure

needs_ffmpeg = pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="ffmpeg is not on PATH")


def test_cache_dir_carries_the_version(tmp_path):
    assert cache_dir(tmp_path) == tmp_path / ".cache" / f"material-v{MATERIAL_VERSION}"
    assert cache_dir().parent.name == ".cache" and cache_dir().parent.parent.name == "tests"


def test_every_hwcycle_file_has_a_recipe():
    for name in ("clip42.h264", "clip42.framemd5", "frame0.i420", "frame800.i420", "clip30.i420", "pic.jpg", "pic640.jpg",
                 "prog.jpg", "two.h264", "two.framemd5", "wide.h264", "wide-idc.h264", "stills", "l-base.h264", "l-base.framemd5",
                 "l-level3.h264", "l-level5.h264", "l-level62.h264",
                 "l-ref8.h264", "l-ref16.h264", "l-rate41.h264", "l-rate42.h264", "l-rate5.h264", "l-1920x1200.h264",
                 "l-3840x2160.framemd5", "l-high10.h264", "l-high422.h264", "pattern.png", "pattern.jpg", "pattern.h264",
                 "two-pattern.h264", "two-pattern.framemd5"):
        assert name in RECIPES, name
    assert "l-level3.framemd5" not in RECIPES  # the l-level streams have no digests of their own


def test_source_beside_the_caches_and_checked(tmp_path):
    cache = cache_dir(tmp_path)
    path = cache.parent / "source" / material.SOURCE_NAME
    path.parent.mkdir(parents=True)
    path.write_bytes(b"not the sample")
    with pytest.raises(MaterialError, match="not the sample's"):
        material.source(cache)


def test_unknown_name_raises(tmp_path):
    with pytest.raises(MaterialError, match="no recipe"):
        ensure("nosuch.bin", tmp_path)
    with pytest.raises(MaterialError, match="no such still"):
        (tmp_path / "stills").mkdir()
        ensure("stills/nosuch.jpg", tmp_path)


def test_pattern_png_and_jpg(tmp_path):
    from PIL import Image
    png = ensure("pattern.png", tmp_path)
    im = Image.open(png)
    assert im.size == (1920, 1080) and im.mode == "RGB"
    jpg = ensure("pattern.jpg", tmp_path)
    assert Image.open(jpg).size == (1920, 1080)
    assert ensure("pattern.png", tmp_path) == png  # cached, nothing redone


@needs_ffmpeg
def test_pattern_stream_decodes_to_the_bars(tmp_path):
    from harness.pictures import judge_bars, pattern
    stream = ensure("pattern.h264", tmp_path)
    assert stream.stat().st_size > 1000
    data = material.ffmpeg("-i", str(stream), "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-")
    shot = np.frombuffer(data, np.uint8).reshape(1080, 1920, 3)
    _, bars = pattern()
    assert judge_bars(shot, bars) == []
    frames = material.sh(["ffprobe", "-v", "error", "-count_frames", "-show_entries", "stream=nb_read_frames",
                          "-of", "csv=p=0", str(stream)]).decode().strip()
    assert frames == "60"


def test_material_ram_uploads_once_by_basename(tmp_path):
    class Device:
        def __init__(self):
            self.uploads = []

        def upload_once(self, local: Path, remote: str) -> None:
            self.uploads.append((local, remote))

    dev = Device()
    mat = Material(tmp_path, dev)
    assert mat.ram("pattern.png") == "RAM:hw/pattern.png"
    assert dev.uploads == [(tmp_path / "pattern.png", "RAM:hw/pattern.png")]
    (tmp_path / "stills").mkdir()
    (tmp_path / "stills" / "j420.jpg").write_bytes(b"x")
    assert mat.ram("stills/j420.jpg") == "RAM:hw/j420.jpg"
