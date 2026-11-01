# SPDX-License-Identifier: BSD-3-Clause
"""
results.py - the output grammar of the Amiga-side rigs (VCTest, VPUPlay), and
the regex checks for the tools that keep their prose (VCInfo, the examples)
and for the debug log.

    output  := "format=1" NL line* verdict
    line    := key "=" value NL        key at column 0: [a-z][a-z0-9_]*("."[a-z0-9_]+)*;
                                       indexed groups by number (stream.0.width);
                                       values: decimal, 0x hex, yes/no, one-decimal
                                       rates (85.3), comma lists, names verbatim
             | "  " prose NL           for humans; kept for the report, never parsed
    verdict := "PASS" NL | "FAIL" NL   the last line

Return codes (AmigaDOS levels): 0 done and the rig's own checks passed (PASS);
5 a self-check failed (fail=<why>); 10 the environment: a library, device,
file or the test hooks missing (fail=); 20 the API refused (error=<code>), an
idle timeout, or a usage error, in which case ReadArgs failed before the
format line was printed and parse() raises HarnessError.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

FORMAT = 1

RC_PASS = 0
RC_CHECK_FAILED = 5
RC_ENVIRONMENT = 10
RC_API = 20

KEY_RE = re.compile(r"^([a-z][a-z0-9_]*(?:\.[a-z0-9_]+)*)=(.*)$")
FORMAT_RE = re.compile(r"^format=(\d+)$")
INT_RE = re.compile(r"^-?\d+$")
FLOAT_RE = re.compile(r"^-?\d+\.\d+$")


class HarnessError(Exception):
    """A defect of the harness or of a rig's output, not of the thing under test."""


@dataclass
class Result:
    """One rig run: its facts, its prose, its verdict and return code."""

    kv: dict[str, str] = field(default_factory=dict)
    prose: list[str] = field(default_factory=list)
    verdict: str | None = None
    rc: int = RC_PASS
    raw: str = ""

    def has(self, key: str) -> bool:
        """Whether the rig printed @key."""
        return key in self.kv

    def num(self, key: str, default: int | float | None = None) -> int | float:
        """The number printed under @key: decimal or 0x hex as int, one-decimal rates as float."""
        if key not in self.kv:
            if default is not None:
                return default
            raise HarnessError(f"{key} missing from the rig's output")
        value = self.kv[key]
        if value.lower().startswith(("0x", "-0x")):
            return int(value, 16)
        if INT_RE.match(value):
            return int(value)
        if FLOAT_RE.match(value):
            return float(value)
        raise HarnessError(f"{key}={value!r} is not a number")

    def flag(self, key: str) -> bool:
        """The yes/no printed under @key."""
        value = self.kv.get(key)
        if value not in ("yes", "no"):
            raise HarnessError(f"{key}={value!r} is not yes or no")
        return value == "yes"

    def group(self, prefix: str) -> list[dict[str, str]]:
        """The indexed group @prefix.N.* as a list of dicts in index order."""
        found: dict[int, dict[str, str]] = {}
        head = re.compile(rf"^{re.escape(prefix)}\.(\d+)\.(.+)$")
        for key, value in self.kv.items():
            m = head.match(key)
            if m:
                found.setdefault(int(m.group(1)), {})[m.group(2)] = value
        return [found[i] for i in sorted(found)]

    @property
    def passed(self) -> bool:
        """Whether the rig ended in PASS with return code 0."""
        return self.verdict == "PASS" and self.rc == RC_PASS


def parse(text: str, rc: int) -> Result:
    """Read a rig's output into a Result; raise HarnessError when it is not in the grammar."""
    lines = [line.rstrip("\r") for line in text.splitlines()]
    nonempty = [i for i, line in enumerate(lines) if line.strip()]
    if not nonempty:
        raise HarnessError("no format line: usage error or wrong tool (empty output)")
    first = FORMAT_RE.match(lines[nonempty[0]])
    if first is None:
        raise HarnessError(f"no format line: usage error or wrong tool ({lines[nonempty[0]]!r})")
    if int(first.group(1)) != FORMAT:
        raise HarnessError(f"output format {first.group(1)}, this harness reads {FORMAT}")
    result = Result(rc=rc, raw=text)
    last = nonempty[-1]
    for i in nonempty[1:]:
        line = lines[i]
        if i == last and line in ("PASS", "FAIL"):
            result.verdict = line
            continue
        m = KEY_RE.match(line)
        if m:
            result.kv[m.group(1)] = m.group(2).strip()
        else:
            result.prose.append(line.strip())
    return result


def _head(text: str, lines: int = 20) -> str:
    """The first @lines lines of @text, for a failure message."""
    return "\n".join(text.splitlines()[:lines])


def expect(text: str, regex: str, what: str) -> re.Match[str]:
    """The match of @regex (multi-line) in @text; AssertionError in plain words when there is none."""
    m = re.search(regex, text, re.M)
    if m is None:
        raise AssertionError(f"{what}: nothing matched /{regex}/ in:\n{_head(text)}")
    return m


def forbid(text: str, regex: str, what: str) -> None:
    """AssertionError in plain words when @regex (multi-line) matches in @text."""
    m = re.search(regex, text, re.M)
    if m is not None:
        raise AssertionError(f"{what}: found {m.group(0)!r} in:\n{_head(text)}")
