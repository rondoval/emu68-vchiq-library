# SPDX-License-Identifier: BSD-3-Clause
"""
perf.py - the report lines of emu68-common's perf.h, as a profile-tier build
writes them to the debug log:

    [<prefix>] <name>: n=<count> sum=<us>us avg=<x>.<y>us max=<us>us

Each line is a delta since the previous report (perf_report() rezeroes), so a
slice's lines are summed per slot and the average recomputed.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

LINE = re.compile(r"^\[(?P<prefix>[\w.-]+)\] (?P<name>[\w.-]+): n=(?P<n>\d+) sum=(?P<sum>\d+)us avg=(?P<avg>[\d.]+)us max=(?P<max>\d+)us")


@dataclass
class Slot:
    """One probe's totals over a slice."""

    n: int = 0
    sum_us: int = 0
    max_us: int = 0

    @property
    def avg_us(self) -> float:
        """Microseconds per event, 0 without events."""
        return self.sum_us / self.n if self.n else 0.0


def slots(text: str, prefix: str = "videocore") -> dict[str, Slot]:
    """The slots of @prefix in a log slice, each summed over its report lines."""
    found: dict[str, Slot] = {}
    for line in text.splitlines():
        m = LINE.match(line)
        if m is None or m.group("prefix") != prefix:
            continue
        slot = found.setdefault(m.group("name"), Slot())
        slot.n += int(m.group("n"))
        slot.sum_us += int(m.group("sum"))
        slot.max_us = max(slot.max_us, int(m.group("max")))
    return found
