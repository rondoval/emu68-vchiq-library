# SPDX-License-Identifier: BSD-3-Clause
"""
report.py - the run's report.md: one row per test with its outcome, time,
notes (record_property) and artifact dir; then the baseline warnings and the
values measured.  pytest's own -ra summary and JUnit XML (--junitxml) exist
beside it; this is the page a person reads after a run.
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Row:
    """One test's outcome."""

    nodeid: str
    outcome: str
    seconds: float
    reason: str = ""
    notes: list[str] = field(default_factory=list)
    artifacts: str = ""


class Report:
    """What the session collects for report.md."""

    def __init__(self, rundir: Path, title: str):
        self.rundir = Path(rundir)
        self.title = title
        self.rows: list[Row] = []
        self.header: list[str] = []
        self.started = time.time()

    def add(self, row: Row) -> None:
        """A finished test."""
        self.rows.append(row)

    def write(self, baselines=None, capture=None) -> Path:
        """report.md into the run dir."""
        counts: dict[str, int] = {}
        for r in self.rows:
            counts[r.outcome] = counts.get(r.outcome, 0) + 1
        lines = [f"# {self.title}", "",
                 f"{len(self.rows)} tests in {time.time() - self.started:.0f} s: " +
                 ", ".join(f"{n} {k}" for k, n in sorted(counts.items())), ""]
        lines += self.header
        if capture is not None:
            lines += ["", f"Debug log: {'captured' if capture.ok else 'NOT captured (' + capture.why + ')'}", ""]
        lines += ["| test | outcome | s | notes |", "|---|---|---|---|"]
        for r in self.rows:
            notes = "; ".join(r.notes)
            if r.reason:
                notes = (r.reason.splitlines()[0] + ("; " + notes if notes else ""))
            lines.append(f"| {r.nodeid} | {r.outcome} | {r.seconds:.0f} | {notes.replace('|', '/')} |")
        if baselines is not None:
            if baselines.warnings:
                lines += ["", "## Baselines", ""] + [f"- {w}" for w in baselines.warnings]
            if baselines.measured:
                lines += ["", "## Measured", "", "| key | value |", "|---|---|"]
                lines += [f"| {k} | {v} |" for k, v in sorted(baselines.measured.items())]
        path = self.rundir / "report.md"
        path.write_text("\n".join(lines) + "\n")
        return path
