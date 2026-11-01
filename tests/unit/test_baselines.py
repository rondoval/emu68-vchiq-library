# SPDX-License-Identifier: BSD-3-Clause
"""The baselines (harness/baselines.py): tolerances both ways, recording, updating the file."""
import json

import pytest

from harness.baselines import Baselines


def test_higher_is_better_absolute():
    b = Baselines("pi4", {"fps": 30.0})
    b.check("fps", 28.5, higher=True, tolerance=2.0)
    b.check("fps", 40.0, higher=True, tolerance=2.0)
    with pytest.raises(AssertionError, match="fps: 27.9 is below the baseline 30.0"):
        b.check("fps", 27.9, higher=True, tolerance=2.0)
    assert b.measured["fps"] == 27.9  # recorded before the verdict


def test_lower_is_better_relative():
    b = Baselines("pi4", {"us": 1000})
    b.check("us", 1250, higher=False, tolerance="25%")
    b.check("us", 10, higher=False, tolerance="25%")
    with pytest.raises(AssertionError, match="above the baseline 1000"):
        b.check("us", 1251, higher=False, tolerance="25%")


def test_bad_tolerance_spec():
    with pytest.raises(ValueError, match="percentage"):
        Baselines.tolerance_of("abc", 1.0)
    assert Baselines.tolerance_of("10%", 200) == 20.0
    assert Baselines.tolerance_of(3, 200) == 3.0


def test_missing_baseline_records_with_warning():
    b = Baselines("cm4", {})
    b.check("fps", 12.5, higher=True, tolerance="10%")
    assert b.measured == {"fps": 12.5}
    assert b.warnings == ["fps: no baseline for cm4, measured 12.5 (recorded only)"]


def test_load_missing_file_and_format(tmp_path):
    b = Baselines.load(tmp_path / "none.json", "pi4")
    assert b.base == {} and b.machine == "pi4"
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"format": 7}))
    with pytest.raises(ValueError, match="format 7"):
        Baselines.load(bad, "pi4")


def test_update_merges_and_keeps_other_machines(tmp_path):
    path = tmp_path / "baselines.json"
    path.write_text(json.dumps({"format": 1, "amiga-pi4": {"b.fps": 30.0, "a.us": 100}, "amiga-cm4": {"b.fps": 20.0}}))
    b = Baselines.load(path, "amiga-pi4")
    assert b.base == {"b.fps": 30.0, "a.us": 100}
    b.check("b.fps", 31.0, higher=True, tolerance=1)
    b.check("c.db", 40.0, higher=True, tolerance=1)
    b.update(path)
    text = path.read_text()
    data = json.loads(text)
    assert data["amiga-pi4"] == {"a.us": 100, "b.fps": 31.0, "c.db": 40.0}
    assert data["amiga-cm4"] == {"b.fps": 20.0}
    assert data["format"] == 1
    assert text.endswith("\n")
    pi4 = text[text.index('"amiga-pi4"'):text.index('"format"')]
    assert pi4.index('"a.us"') < pi4.index('"b.fps"') < pi4.index('"c.db"')  # sorted keys
    assert text.index('"amiga-cm4"') < text.index('"amiga-pi4"') < text.index('"format"')


def test_write_measured(tmp_path):
    b = Baselines("amiga-pi4", {})
    b.check("z", 1, higher=True, tolerance=1)
    b.check("a", 2, higher=True, tolerance=1)
    out = tmp_path / "measured.json"
    b.write_measured(out)
    data = json.loads(out.read_text())
    assert data == {"format": 1, "machine": "amiga-pi4", "measured": {"a": 2, "z": 1}}
    assert out.read_text().index('"a"') < out.read_text().index('"z"')
