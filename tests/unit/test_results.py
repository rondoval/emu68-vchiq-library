# SPDX-License-Identifier: BSD-3-Clause
"""The rig output grammar (harness/results.py) on captured and hand-written samples."""
from pathlib import Path

import pytest

from harness import results
from harness.results import HarnessError, Result, expect, forbid, parse

SAMPLES = Path(__file__).parent / "samples"


def sample(name: str) -> str:
    return (SAMPLES / name).read_text()


def test_rc_constants_are_amigados_levels():
    assert (results.RC_PASS, results.RC_CHECK_FAILED, results.RC_ENVIRONMENT, results.RC_API) == (0, 5, 10, 20)
    assert results.FORMAT == 1


def test_decode_sample_facts():
    r = parse(sample("vctest-decode.txt"), 0)
    assert r.passed
    assert r.verdict == "PASS" and r.rc == 0
    assert r.kv["command"] == "DECODE"
    assert r.num("cli") == 3
    assert r.num("decode.frames") == 300
    assert r.num("decode.fps") == 86.8
    assert isinstance(r.num("decode.fps"), float)
    assert r.num("stream.0.colorspace") == 1  # hex
    assert r.flag("decode.eos") is True
    assert r.flag("decode.idle") is False
    assert r.num("ref.mismatches") == 0
    assert r.kv["videocodec.version"] == "1.4"


def test_decode_sample_groups_and_prose():
    r = parse(sample("vctest-decode.txt"), 0)
    streams = r.group("stream")
    assert len(streams) == 1
    assert streams[0]["width"] == "1920" and streams[0]["mem_height"] == "1088"
    assert r.group("frame") == []  # frame.first.* has no numeric index
    assert r.prose == ["first frame: 1920x1080 I420, 3133440 bytes, planes at 0, 2088960, 2611200",
                       "300 frames compared, 0 mismatches"]
    assert r.raw.startswith("format=1")


def test_vpuplay_sample_indexed_groups_in_order():
    r = parse(sample("vpuplay.txt"), 0)
    outputs = r.group("output")
    assert [o["width"] for o in outputs] == ["1920", "960"]
    assert r.num("screen.mode") == 0x50031303
    assert r.num("play.fps") == 29.9
    assert r.num("play.total_us") == 10034123
    assert r.has("window.moves") and not r.has("window.closes")
    assert r.num("window.closes", default=0) == 0


def test_fail_sample():
    r = parse(sample("vctest-fail.txt"), 20)
    assert r.verdict == "FAIL" and r.rc == 20 and not r.passed
    assert r.num("error") == -6
    assert r.kv["fail"] == "VC_DecodeImage refused the picture"


def test_usage_error_raises():
    with pytest.raises(HarnessError, match="no format line"):
        parse(sample("usage-error.txt"), 20)
    with pytest.raises(HarnessError, match="no format line"):
        parse("", 0)


def test_wrong_format_version_raises():
    with pytest.raises(HarnessError, match="format 2"):
        parse("format=2\nPASS\n", 0)


def test_tolerant_lines_and_repeated_keys():
    text = "format=1\nUnexpected prose\nkey=first\nkey=second\n  note\nPASS\n"
    r = parse(text, 0)
    assert r.kv["key"] == "second"
    assert "Unexpected prose" in r.prose and "note" in r.prose


def test_verdict_only_as_last_line():
    r = parse("format=1\nPASS\nkey=1\n", 0)
    assert r.verdict is None and "PASS" in r.prose
    r = parse("format=1\nkey=1\nFAIL\n\n", 5)
    assert r.verdict == "FAIL"


def test_cr_line_endings():
    r = parse("format=1\r\ndecode.fps=85.3\r\nPASS\r\n", 0)
    assert r.num("decode.fps") == 85.3 and r.verdict == "PASS"


def test_num_errors_in_plain_words():
    r = Result(kv={"x": "abc"})
    with pytest.raises(HarnessError, match="not a number"):
        r.num("x")
    with pytest.raises(HarnessError, match="missing"):
        r.num("y")
    with pytest.raises(HarnessError, match="not yes or no"):
        r.flag("x")


def test_expect_and_forbid():
    text = "line one\n  vchiq.device: 1.4 connected\nPASS\n"
    assert expect(text, r"^  vchiq\.device: .*connected", "the summary").group(0).endswith("connected")
    with pytest.raises(AssertionError, match="the summary: nothing matched"):
        expect(text, r"^FAIL$", "the summary")
    forbid(text, r"stray", "the summary")
    with pytest.raises(AssertionError, match="found 'PASS'"):
        forbid(text, r"^PASS$", "the summary")
