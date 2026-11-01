# SPDX-License-Identifier: BSD-3-Clause
"""The picture maths (harness/pictures.py) on synthetic arrays and the colour-bar pattern."""
import math

import numpy as np
import pytest

from harness import pictures
from harness.pictures import (BAR_COLOURS, bar_means, judge_bars, load_ppm, pattern, psnr, rgb565_pixels, still_pixels,
                              y_planes)


def test_psnr_identical_is_99():
    a = np.full((4, 4, 3), 77, np.uint8)
    assert psnr(a, a.copy()) == 99.0


def test_psnr_known_noise():
    a = np.zeros((8, 8, 3), np.uint8)
    b = np.full((8, 8, 3), 16, np.uint8)  # mse 256
    assert math.isclose(psnr(a, b), 10 * math.log10(255.0 * 255.0 / 256), rel_tol=1e-9)


def test_psnr_shape_mismatch_fails_in_plain_words():
    with pytest.raises(AssertionError, match="size"):
        psnr(np.zeros((2, 2, 3), np.uint8), np.zeros((2, 3, 3), np.uint8))


def test_pattern_geometry():
    picture, bars = pattern()
    assert picture.shape == (1080, 1920, 3) and picture.dtype == np.uint8
    assert len(bars) == 8
    for bar, rgb in zip(bars, BAR_COLOURS):
        assert bar.rgb == rgb
        assert bar.x % 16 == 0 and bar.w % 16 == 0 and bar.h % 16 == 0 and bar.y == 0
        assert (picture[:bar.h, bar.x:bar.x + bar.w] == rgb).all()
    assert bars[-1].x + bars[-1].w == 1920
    assert bars[0].h == 800  # three quarters of 1080, rounded down to 16s
    ramp = picture[bars[0].h:]
    assert (ramp[:, 0] == 0).all() and (ramp[:, -1] == 255).all()
    assert (ramp[0, :, 0] == ramp[0, :, 1]).all() and (np.diff(ramp[0, :, 0].astype(int)) >= 0).all()


def test_pattern_other_size_keeps_edges_on_16():
    picture, bars = pattern(640, 480)
    assert picture.shape == (480, 640, 3)
    assert all(b.x % 16 == 0 and b.w % 16 == 0 for b in bars) and bars[0].h == 352


def test_judge_bars_passes_on_the_pattern():
    picture, bars = pattern()
    assert judge_bars(picture, bars) == []
    means = bar_means(picture, bars)
    assert means[1][0] == (255.0, 255.0, 0.0) and means[1][1] == 0.0


def test_judge_bars_catches_swapped_channels():
    picture, bars = pattern()
    swapped = picture[:, :, ::-1].copy()  # red and blue exchanged
    problems = judge_bars(swapped, bars)
    assert any(p.startswith("bar 1 ") for p in problems)  # yellow became cyan
    assert all("off by" in p for p in problems)
    assert not any(p.startswith("bar 0 ") for p in problems)  # white and grey survive a swap
    assert not any(p.startswith("bar 7 ") for p in problems)


def test_judge_bars_catches_stripes():
    picture, bars = pattern()
    striped = picture.copy()
    bar = bars[6]
    striped[0:bar.h:2, bar.x:bar.x + bar.w] = (255, 255, 255)  # every other row white in the blue bar
    problems = judge_bars(striped, bars)
    assert any(p.startswith("bar 6 ") and "not flat" in p for p in problems)


def test_judge_bars_with_scale_and_origin():
    picture, bars = pattern()
    shot = np.zeros((600, 1000, 3), np.uint8)
    small = picture[::2, ::2]  # half size, placed at (20, 40)
    shot[40:40 + 540, 20:20 + 960] = small
    assert judge_bars(shot, bars, scale=0.5, origin=(20, 40)) == []
    assert judge_bars(shot, bars, scale=0.5, origin=(0, 0)) != []


def test_rgb565_decode():
    r, g, b = 200, 100, 50
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    data = np.array([v, v], "<u2").tobytes()
    px = rgb565_pixels(data, 2, 1)
    assert px.shape == (1, 2, 3)
    assert tuple(px[0, 0]) == (r & ~7, g & ~3, b & ~7)


def test_load_ppm_with_comment(tmp_path):
    pixels = np.arange(2 * 3 * 3, dtype=np.uint8).reshape(3, 2, 3)
    path = tmp_path / "pic.ppm"
    path.write_bytes(b"P6\n# a comment\n2 3\n255\n" + pixels.tobytes())
    assert (load_ppm(path) == pixels).all()


def test_still_pixels_formats():
    rgb = np.arange(2 * 2 * 3, dtype=np.uint8).reshape(2, 2, 3)
    ppm = b"P6 2 2 255\n" + rgb.tobytes()
    out, alpha = still_pixels("RGB24", 2, 2, ppm)
    assert (out == rgb).all() and alpha is None
    out, alpha = still_pixels("BGR24", 2, 2, rgb[:, :, ::-1].tobytes())
    assert (out == rgb).all()
    rgba = np.dstack([rgb, np.full((2, 2), 9, np.uint8)])
    out, alpha = still_pixels("RGBA32", 2, 2, rgba.tobytes())
    assert (out == rgb).all() and (alpha == 9).all()
    bgra = rgba[:, :, [2, 1, 0, 3]]
    out, alpha = still_pixels("BGRA32", 2, 2, bgra.tobytes())
    assert (out == rgb).all() and (alpha == 9).all()


def test_y_planes():
    w, h = 4, 2
    frame = bytes(range(w * h)) + bytes(w * h // 2)
    planes = y_planes(frame * 2, w, h, 2)
    assert len(planes) == 2 and planes[1].shape == (h, w) and planes[0][1, 3] == 7


def test_module_has_the_moved_helpers():
    for name in ("load_ppm", "psnr", "ref_rgb", "best_psnr", "rgb565_picture", "y_planes", "key_frames", "still_pixels"):
        assert callable(getattr(pictures, name))
