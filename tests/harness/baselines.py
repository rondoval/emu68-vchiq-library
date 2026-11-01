# SPDX-License-Identifier: BSD-3-Clause
"""
baselines.py - measured numbers per machine, checked with a tolerance.

tests/baselines.json holds numbers only:

    {"format": 1,
     "amiga-pi4": {"decode_clip42.decode.fps": 85.3, "jpeg_1080p.psnr_db": 34.1},
     "amiga-cm4": {}}

What a number means (unit, direction, tolerance) is said where it is checked,
in the test: baselines.check("jpeg_1080p.psnr_db", score, higher=True,
tolerance=2.0).  A run records every value it measured; with
--update-baselines the harness writes this machine's measured values back,
and the user reads the diff before committing.
"""
from __future__ import annotations

import json
from pathlib import Path

FORMAT = 1


class Baselines:
    """The baselines of one machine, the values this run measured, and the warnings."""

    def __init__(self, machine: str, base: dict[str, float] | None = None):
        self.machine = machine
        self.base: dict[str, float] = dict(base or {})
        self.measured: dict[str, float] = {}
        self.warnings: list[str] = []

    @classmethod
    def load(cls, path: str | Path, machine: str) -> "Baselines":
        """The baselines of @machine from the JSON at @path; none when the file does not exist."""
        path = Path(path)
        if not path.exists():
            return cls(machine)
        data = json.loads(path.read_text())
        if data.get("format") != FORMAT:
            raise ValueError(f"{path}: format {data.get('format')!r}, this harness reads {FORMAT}")
        return cls(machine, data.get(machine, {}))

    @staticmethod
    def tolerance_of(spec: float | str, base: float) -> float:
        """An absolute tolerance from a number, or from a 'N%' string relative to @base."""
        if isinstance(spec, str):
            if not spec.endswith("%"):
                raise ValueError(f"tolerance {spec!r}: a number, or a percentage like '10%'")
            return abs(base) * float(spec[:-1]) / 100.0
        return float(spec)

    def check(self, key: str, value: float, higher: bool, tolerance: float | str) -> None:
        """Record @value under @key and compare it with the machine's baseline.

        @higher says which way is better; the value may fall short of the
        baseline by @tolerance (a number, or 'N%' of the baseline) when higher
        is better, or exceed it by that much otherwise.  Without a baseline
        the value is only recorded and a warning kept.
        """
        self.measured[key] = value
        if key not in self.base:
            self.warnings.append(f"{key}: no baseline for {self.machine}, measured {value} (recorded only)")
            return
        base = self.base[key]
        tol = self.tolerance_of(tolerance, base)
        if higher and value < base - tol:
            raise AssertionError(f"{key}: {value} is below the baseline {base} by more than {tol:g} "
                                 f"(higher is better, tolerance {tolerance})")
        if not higher and value > base + tol:
            raise AssertionError(f"{key}: {value} is above the baseline {base} by more than {tol:g} "
                                 f"(lower is better, tolerance {tolerance})")

    def write_measured(self, path: str | Path) -> None:
        """The values this run measured, as JSON with sorted keys."""
        Path(path).write_text(json.dumps({"format": FORMAT, "machine": self.machine, "measured": self.measured},
                                         indent=2, sort_keys=True) + "\n")

    def update(self, path: str | Path) -> None:
        """Merge this machine's measured values into the baselines file at @path, keeping the other machines."""
        path = Path(path)
        data = json.loads(path.read_text()) if path.exists() else {}
        data["format"] = FORMAT
        machine = dict(data.get(self.machine, {}))
        machine.update(self.measured)
        data[self.machine] = machine
        path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
