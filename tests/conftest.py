# SPDX-License-Identifier: BSD-3-Clause
"""
conftest.py - the suite's options and fixtures (tests/README.md).

Session: build_info (the installed build's flavour from CMakeCache.txt),
rundir, capture (the debug log), device (connected, installed, RAM:hw ready),
material, baselines, operator.  Function: testlog (every test's artifact dir
and log slice), debuglog and driver_log (skip without a capture or a logging
build), artifacts.
"""
from __future__ import annotations

import json
import os
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS))

from harness.amiga import RESIDENT, Device, HarnessError, Unreachable, client  # noqa: E402
from harness.baselines import Baselines  # noqa: E402
from harness.material import Material, cache_dir  # noqa: E402
from harness.report import Report, Row  # noqa: E402
from harness.serial import DebugLog  # noqa: E402

STACK = TESTS.parents[2]
COMPONENT = "emu68-vchiq-library"


# -- options

def pytest_addoption(parser):
    g = parser.getgroup("amiga", "the machine under test")
    g.addoption("--server", default="amiga-pi4", help="the amimcp server name in ~/.claude.json (amiga-pi4, amiga-cm4); also the baselines key")
    g.addoption("--amimcp", default=str(STACK.parent / "amimcp"), help="the amimcp checkout (its server/amiga.py), by default beside the stack's")
    g.addoption("--install-dir", default=str(STACK / "install"), help="the stack's install tree")
    g.addoption("--build-dir", default=str(STACK / "build"), help="the stack's build dir (CMakeCache.txt: the flavour)")
    g.addoption("--run-dir", default=None, help="where this run's report, log and artifacts go (default tests/runs/<timestamp>)")
    g.addoption("--no-install", action="store_true", help="do not replace the resident files; check they match the install tree")
    g.addoption("--no-reboot", action="store_true", help="deselect the tests that reboot")
    g.addoption("--listen", action="store_true", help="select the tests someone at the machine judges by ear")
    g.addoption("--hands", action="store_true", help="select the tests that need the user's hands")
    g.addoption("--profile", action="store_true", help="select the timing tests (a profile-tier build installed)")
    g.addoption("--serial", default="/dev/ttyUSB0", help="the debug log's serial device, or none")
    g.addoption("--require-debug-log", action="store_true", help="fail, not skip, the tests that need the debug log when it is missing")
    g.addoption("--update-baselines", action="store_true", help="write this machine's measured values into baselines.json")
    g.addoption("--keep-ram", action="store_true", help="leave RAM:hw on the Amiga at the end")


@dataclass
class BuildInfo:
    """The installed build's flavour, from the build dir's CMakeCache.txt."""

    backend: str = "?"
    tier: str = "?"
    debug_pins: str = ""
    profile_pins: str = ""
    hooks: bool = False
    stamp: str = ""

    @classmethod
    def read(cls, build_dir: Path) -> "BuildInfo":
        cache = Path(build_dir) / "CMakeCache.txt"
        info = cls()
        if not cache.exists():
            return info
        values = dict(re.findall(r"^(EMU68_\w+):\w+=(.*)$", cache.read_text(), re.M))
        info.backend = values.get("EMU68_DEBUG_BACKEND", "?")
        info.tier = values.get("EMU68_TIER", "?")
        info.debug_pins = values.get("EMU68_DEBUG", "")
        info.profile_pins = values.get("EMU68_PROFILE", "")
        info.hooks = values.get("EMU68_VCHIQ_TEST_HOOKS", "OFF").upper() in ("ON", "1", "TRUE")
        info.stamp = values.get("EMU68_BUILD_STAMP", "")
        return info

    def _pinned(self, pins: str) -> bool:
        return COMPONENT in pins.split(";") or "ALL" in pins.split(";")

    @property
    def debug(self) -> bool:
        """Whether the component logs to the debug backend."""
        return self.backend != "off" and (self.tier in ("debug", "trace") or self._pinned(self.debug_pins))

    @property
    def profile(self) -> bool:
        """Whether the component's perf probes are compiled in."""
        return self.backend != "off" and (self.tier in ("profile", "debug", "trace") or self._pinned(self.profile_pins) or
                                          self._pinned(self.debug_pins))


class Session:
    """What the hooks share with the fixtures."""

    def __init__(self):
        self.build_info: BuildInfo | None = None
        self.report: Report | None = None
        self.power_seen: list[str] = []
        self.baselines: Baselines | None = None
        self.capture: DebugLog | None = None
        self.rundir: Path | None = None


def pytest_configure(config):
    config._vc = Session()
    config._vc.build_info = BuildInfo.read(Path(config.getoption("--build-dir")))


def pytest_collection_modifyitems(config, items):
    """Deselect the tiers not asked for, skip what the installed build cannot do, run the reboots last."""
    info = config._vc.build_info
    kept, dropped = [], []
    for item in items:
        if item.get_closest_marker("listen") and not config.getoption("--listen"):
            dropped.append(item)
        elif item.get_closest_marker("hands") and not config.getoption("--hands"):
            dropped.append(item)
        elif item.get_closest_marker("profile") and not config.getoption("--profile"):
            dropped.append(item)
        elif item.get_closest_marker("reboot") and config.getoption("--no-reboot"):
            dropped.append(item)
        else:
            kept.append(item)
    for item in kept:
        if item.get_closest_marker("hooks") and not info.hooks:
            item.add_marker(pytest.mark.skip(reason="the installed build has no test hooks (EMU68_VCHIQ_TEST_HOOKS)"))
        if item.get_closest_marker("profile") and not info.profile:
            item.add_marker(pytest.mark.skip(reason="the installed build does not profile this component"))
    kept.sort(key=lambda it: (bool(it.get_closest_marker("reboot")), bool(it.get_closest_marker("hands"))))
    if dropped:
        config.hook.pytest_deselected(items=dropped)
    items[:] = kept


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    if call.excinfo is not None and isinstance(call.excinfo.value, Unreachable):
        pytest.exit(f"the Amiga is unreachable: {call.excinfo.value}", returncode=3)
    report = item.config._vc.report
    if report is None:
        return
    if rep.when == "call" or (rep.when == "setup" and rep.outcome != "passed"):
        reason = ""
        if rep.outcome == "skipped" and isinstance(rep.longrepr, tuple):
            reason = rep.longrepr[2]
        elif rep.outcome != "passed":
            reason = str(rep.longrepr).splitlines()[-1] if rep.longrepr else ""
        notes = [f"{k}: {v}" for k, v in item.user_properties]
        artifacts = str(getattr(item, "_artifacts", ""))
        outcome = rep.outcome if rep.when == "call" or rep.outcome == "skipped" else f"{rep.outcome} in setup"
        report.add(Row(item.nodeid, outcome, rep.duration, reason, notes, artifacts))


def pytest_sessionfinish(session, exitstatus):
    vc = session.config._vc
    if vc.report is not None:
        if vc.capture is not None and vc.capture.ok:
            from harness.serial import garbled
            bad, total = garbled(vc.capture.text(0))
            vc.report.header.append(f"Capture quality: {bad} of {total} lines garbled ({100 * bad / max(1, total):.1f}%: "
                                    f"Emu68's own output interleaved with the drivers')")
        path = vc.report.write(vc.baselines, vc.capture)
        session.config.pluginmanager.get_plugin("terminalreporter").write_line(f"report: {path}")


# -- session fixtures

@pytest.fixture(scope="session")
def build_info(request) -> BuildInfo:
    """The installed build's flavour."""
    return request.config._vc.build_info


@pytest.fixture(scope="session")
def rundir(request) -> Path:
    """This run's directory: report.md, serial.log, measured.json, one dir per test."""
    chosen = request.config.getoption("--run-dir")
    path = Path(chosen) if chosen else TESTS / "runs" / time.strftime("%Y%m%d-%H%M%S")
    path.mkdir(parents=True, exist_ok=True)
    request.config._vc.rundir = path
    request.config._vc.report = Report(path, f"emu68-vchiq-library tests, {time.strftime('%Y-%m-%d %H:%M')}")
    info = request.config._vc.build_info
    (path / "run.json").write_text(json.dumps({"server": request.config.getoption("--server"), "build": info.__dict__,
                                               "options": {k: v for k, v in vars(request.config.option).items()
                                                           if k in ("no_install", "no_reboot", "listen", "hands", "serial",
                                                                    "update_baselines", "keep_ram")}}, indent=2) + "\n")
    return path


@pytest.fixture(scope="session")
def capture(request, rundir) -> DebugLog:
    """The debug log, captured for the whole session; its .ok says whether the port opened."""
    log = DebugLog(rundir / "serial.log", request.config.getoption("--serial"))
    log.start()
    request.config._vc.capture = log
    yield log
    log.stop()


@pytest.fixture(scope="session")
def baselines(request, rundir) -> Baselines:
    """This machine's baselines; the measured values are written at the end (and back into baselines.json with --update-baselines)."""
    b = Baselines.load(TESTS / "baselines.json", request.config.getoption("--server"))
    request.config._vc.baselines = b
    yield b
    b.write_measured(rundir / "measured.json")
    if request.config.getoption("--update-baselines"):
        b.update(TESTS / "baselines.json")


def _install(device: Device, report: Report, capture: DebugLog, info: BuildInfo) -> None:
    """Replace the resident files with the install tree's, reboot, read them back (twice if PFS3 lost one)."""
    files = [(remote, (device.install_dir / local).read_bytes()) for local, remote in RESIDENT
             if (device.install_dir / local).exists()]
    device.install_mark = None
    missing = [remote for local, remote in RESIDENT if not (device.install_dir / local).exists()]
    if missing:
        report.header.append("Not built, not replaced: " + ", ".join(missing))
    todo = files
    mark = capture.mark()
    for wait in (20, 60):
        for remote, data in todo:
            device.upload_bytes(remote, data)
        time.sleep(wait)  # PFS3 delays its writes: a reset right after an upload loses it
        device.reboot()
        todo = [(remote, data) for remote, data in files if device.download(remote) != data]
        if not todo:
            break
    if todo:
        pytest.exit("lost in the reboot again: " + ", ".join(r for r, _ in todo), returncode=3)
    device.replaced = [remote for remote, _ in files]
    report.header.append("Replaced on the Amiga: " + ", ".join(device.replaced))
    device.install_mark = mark


def _verify_installed(device: Device, report: Report) -> None:
    """With --no-install: the resident files on the Amiga must be the install tree's."""
    differ = [remote for local, remote in RESIDENT
              if (device.install_dir / local).exists() and device.download(remote) != (device.install_dir / local).read_bytes()]
    if differ:
        pytest.exit("installed files differ from the install tree (run without --no-install): " + ", ".join(differ), returncode=3)
    report.header.append("Installed files checked against the install tree: " + ", ".join(r for _, r in RESIDENT))


@pytest.fixture(scope="session")
def device(request, rundir, capture, build_info) -> Device:
    """The Amiga: connected, the resident files installed (unless --no-install), RAM:hw ready, the rigs on it."""
    mod, png, ami = client(request.config.getoption("--amimcp"), request.config.getoption("--server"))
    dev = Device(mod, png, ami, Path(request.config.getoption("--install-dir")), capture, rundir)
    report = request.config._vc.report
    try:
        dev.ami.ping()
    except Exception as e:
        pytest.exit(f"the agent on {request.config.getoption('--server')} does not answer: {e}", returncode=3)
    rc, text = dev.exec("List >NIL: C:Reboot C:MakeDir C:Break C:Join C:Status C:Avail", timeout=15)
    if rc != 0:
        pytest.exit(f"C: is not as expected (Assign C: EXISTS): {text.strip()}", returncode=3)
    if dev.exec("List >NIL: LIBS:videocodec-tests", timeout=15)[0] == 0:  # left by an interrupted run
        dev.exec("Rename LIBS:videocodec-tests AS LIBS:videocodec", timeout=15)
    dev.ram_dirs()
    if request.config.getoption("--no-install"):
        _verify_installed(dev, report)
    else:
        _install(dev, report, capture, build_info)
    # the capture self-test: the rig's mark must show in the log
    mark = capture.mark()
    r = dev.run("VCTest", "MARK hello", 30)
    if not (r.passed and r.kv.get("mark") == "hello"):
        pytest.exit(f"VCTest does not run on the Amiga:\n{r.raw[-400:]}", returncode=3)
    if capture.ok and not capture.wait_for(r"\[VCTest\] mark hello", mark, 3.0):
        capture.ok = False
        capture.why = "the log shows no mark from the Amiga: another machine's adapter, or the wrong port"
        report.header.append("Debug log: port open but no mark from this Amiga; log checks skipped")
    # the flavour proof: the device's first open after the install reboot logs when the build is the debug one
    if capture.ok and build_info.debug and getattr(dev, "install_mark", None) is not None:
        dev.run("VCTest", "STATUS", 30)
        if not capture.wait_for(r"\[vchiq\] up: session", dev.install_mark, 3.0):
            pytest.exit("the installed build does not log ('[vchiq] up:' missing after the reboot): not the debug flavour?",
                        returncode=3)
    yield dev
    if not request.config.getoption("--keep-ram"):
        dev.exec("Delete RAM:hw ALL QUIET", timeout=60)


@pytest.fixture(scope="session")
def material(device) -> Material:
    """The test material: made on the host when missing, uploaded to RAM:hw when first used."""
    return Material(cache_dir(TESTS), device)


class Operator:
    """Someone at the machine, asked through the terminal."""

    def __init__(self, enabled: bool, capmanager):
        self.enabled = enabled
        self.capmanager = capmanager

    def say(self, text: str) -> None:
        """A line for the operator, past pytest's capture."""
        with self.capmanager.global_and_fixture_disabled():
            print(f"\n>>> {text}", flush=True)

    def ask(self, question: str) -> None:
        """A yes/no question; n fails the test, s skips it."""
        if not self.enabled:
            pytest.skip("needs someone at the machine (--listen or --hands)")
        if not sys.stdin.isatty():
            pytest.skip("no terminal to ask the operator")
        with self.capmanager.global_and_fixture_disabled():
            answer = input(f"\n>>> {question} [y/n/s] ").strip().lower()
        if answer == "s":
            pytest.skip("the operator skipped it")
        if answer != "y":
            pytest.fail(f"the operator said no: {question}")


@pytest.fixture(scope="session")
def operator(request) -> Operator:
    """The person at the machine, for the listen and hands tiers."""
    enabled = request.config.getoption("--listen") or request.config.getoption("--hands")
    return Operator(enabled, request.config.pluginmanager.getplugin("capturemanager"))


# -- function fixtures

@pytest.fixture(scope="module", autouse=True)
def power_watch(request):
    """After each module, what the firmware saw of the Pi's power: dips show here, and a restart they caused clears them."""
    if request.path.parent.name == "unit":
        yield
        return
    device = request.getfixturevalue("device")
    yield
    vc = request.config._vc
    try:
        seen = device.power()
    except HarnessError:
        return  # the agent stalled: the next module asks again
    if seen and seen != vc.power_seen:
        vc.power_seen = seen
        vc.report.header.append(f"Power: {', '.join(seen)} since the Pi started, seen after {request.path.name}")


@pytest.fixture(autouse=True)
def testlog(request):
    """A hardware test's artifact dir and debug-log slice, saved at the end; nothing for the host-only tests."""
    if "device" not in request.fixturenames:
        yield None
        return
    rundir = request.getfixturevalue("rundir")
    capture = request.getfixturevalue("capture")
    name = re.sub(r"[^A-Za-z0-9_.\[\]-]+", "_", request.node.name)
    path = rundir / request.node.module.__name__ / name
    path.mkdir(parents=True, exist_ok=True)
    request.node._artifacts = path
    mark = capture.mark()
    yield path
    funcargs = getattr(request.node, "funcargs", None)  # None when pytest.exit() ended the run: the teardown comes after
    dev = funcargs.get("device") if funcargs else None
    if dev is not None:
        dev.test_name = "session"
        dev.artifacts = None
    if capture.ok:
        text = capture.text(mark)
        if text:
            (path / "debug.log").write_text(text, encoding="latin-1")


@pytest.fixture(autouse=True)
def _name_device(request, testlog):
    """Tell the device which test runs, so its output files and screenshots land in the test's artifact dir."""
    dev = request.node.funcargs.get("device") if hasattr(request.node, "funcargs") else None
    if dev is not None:
        dev.test_name = request.node.name
        dev.artifacts = testlog
        dev.runs = 0
    yield


@pytest.fixture
def artifacts(request) -> Path:
    """This test's artifact dir."""
    return request.node._artifacts


@pytest.fixture
def debuglog(request, capture) -> DebugLog:
    """The debug log capture, for a test that reads the rigs' marks; skipped (or failed with --require-debug-log) without one."""
    if not capture.ok:
        if request.config.getoption("--require-debug-log"):
            pytest.fail(f"no debug log capture: {capture.why}")
        pytest.skip(f"no debug log capture: {capture.why}")
    return capture


@pytest.fixture
def driver_log(request, debuglog, build_info) -> DebugLog:
    """The debug log, for a test that reads the drivers' lines: needs the debug flavour installed too."""
    if not build_info.debug:
        pytest.skip("the installed build does not log (not the debug flavour)")
    return debuglog
