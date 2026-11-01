# SPDX-License-Identifier: BSD-3-Clause
"""
amiga.py - the device under test, reached through amimcp's Python client
(its server/amiga.py): commands, the rigs' runs, uploads with read-back,
background jobs, reboots, screenshots.

The agent serves nothing while a command is within its deadline, so a rig
that runs longer than a minute is started with Run >NIL: and its output file
is polled (Job).  Everything temporary lives in RAM:hw: the rigs in
RAM:hw/bin, their outputs in RAM:hw/out, the material beside them.  A reboot
empties RAM:, so the device forgets what it uploaded (ram_known) and the
material and the rigs go up again on their next use.
"""
from __future__ import annotations

import json
import os
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from .results import HarnessError, Result, parse

RAM = "RAM:hw"
BIN = f"{RAM}/bin"
OUT = f"{RAM}/out"
PIECE = 12 << 20  # one agent frame carries at most 16 MiB: a larger file goes in pieces, joined on the Amiga
KV_TOOLS = ("VCTest", "VPUPlay")  # the rigs that speak the grammar and write begin/end marks

# where the install tree keeps each rig, and the examples
TOOLS = {
    "VCTest": "Developer/Tools/VCTest",
    "VPUPlay": "Developer/Tools/VPUPlay",
    "VCPlay": "Developer/Examples/videocodec/VCPlay",
    "VCShow": "Developer/Examples/videocodec/VCShow",
}

# what is installed outside C:, so the shell needs its full name
PLACED = {"VCGrab": "SYS:Tools/Commodities/VCGrab"}

# the files the suite replaces on the Amiga, from the install tree
RESIDENT = [
    ("DEVS/vchiq.device", "DEVS:vchiq.device"),
    ("LIBS/videocodec.library", "LIBS:videocodec.library"),
    ("LIBS/videocodec/videocore.codec", "LIBS:videocodec/videocore.codec"),
    ("C/VCInfo", "C:VCInfo"),
    ("Tools/Commodities/VCGrab", "SYS:Tools/Commodities/VCGrab"),
    ("Tools/Commodities/VCGrab.info", "SYS:Tools/Commodities/VCGrab.info"),
    ("DEVS/AHI/videocore.audio", "DEVS:AHI/videocore.audio"),
    ("DEVS/AudioModes/VIDEOCORE", "DEVS:AudioModes/VIDEOCORE"),
    ("Classes/DataTypes/vcjpeg.datatype", "SYS:Classes/DataTypes/vcjpeg.datatype"),
    ("Storage/DataTypes/VCJPEG", "SYS:Storage/DataTypes/VCJPEG"),
]


POWER_EVENTS = {0x10000: "under-voltage", 0x20000: "ARM frequency capped", 0x40000: "throttled", 0x80000: "soft temperature limit"}


class Unreachable(HarnessError):
    """The agent did not answer, even after waiting for it: the run cannot go on."""


class ToolTimeout(AssertionError):
    """A rig ran past its deadline and was broken; the message carries what it had printed."""


def client(amimcp: str, server: str):
    """amimcp's module and a connected client for @server (its address and token from ~/.claude.json, or AMIGA_*)."""
    sys.path.insert(0, os.path.join(amimcp, "server"))
    import amiga  # noqa: E402
    import png  # noqa: E402

    host = os.environ.get("AMIGA_HOST")
    token = os.environ.get("AMIGA_TOKEN", "")
    port = int(os.environ.get("AMIGA_PORT", "7846"))
    if not host:
        cfg = json.load(open(os.path.expanduser("~/.claude.json")))
        env = cfg["mcpServers"][server]["env"]
        host, token, port = env["AMIGA_HOST"], env.get("AMIGA_TOKEN", ""), int(env.get("AMIGA_PORT", "7846"))
    return amiga, png, amiga.Amiga(host=host, port=port, token=token, timeout=120.0)


@dataclass
class Job:
    """A rig started with Run >NIL:, its output in a RAM: file."""

    device: "Device"
    tool: str
    out: str
    started: float = field(default_factory=time.time)
    mark: int = 0

    def output(self) -> str:
        """What the rig has printed so far."""
        try:
            return self.device.download(self.out).decode("latin-1", "replace")
        except self.device.mod.AmigaError:
            return ""

    def done(self) -> bool:
        """Whether the rig has ended: its verdict line is in the file, or (an example) its process is gone."""
        text = self.output()
        if self.tool in KV_TOOLS:
            return bool(re.search(r"^(PASS|FAIL)\s*$", text, re.M))
        return not self.device.procs(self.tool)

    def wait(self, timeout: float) -> Result | str:
        """Poll until the rig ends; its parsed Result (a kv rig) or its text (an example).  ToolTimeout after @timeout."""
        t0 = time.time()
        while not self.done():
            if time.time() - t0 > timeout:
                self.stop()
                raise ToolTimeout(f"{self.tool} still running after {timeout} s:\n{self.output()[-1200:]}")
            time.sleep(1)
        text = self.output()
        self.device.log_settle(self.mark, self.tool)
        self.device.keep_output(self.out, text)
        if self.tool in KV_TOOLS:
            return parse(text, self.device.rc_of(text))
        return text

    def stop(self, timeout: float = 20) -> None:
        """Ctrl-C to the rig (its process found by its command name) and wait for it to end."""
        for n in self.device.procs(self.tool):
            self.device.exec(f"Break {n} C", timeout=10)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.done() and not self.device.procs(self.tool):
                return
            time.sleep(1)
        raise AssertionError(f"{self.tool} did not stop on Ctrl-C within {timeout} s")


class Device:
    """One Amiga, through the agent."""

    def __init__(self, mod, png, ami, install_dir: Path, capture, rundir: Path):
        self.mod = mod
        self.png = png
        self.ami = ami
        self.install_dir = Path(install_dir)
        self.capture = capture
        self.rundir = Path(rundir)
        self.ram_known: set[str] = set()
        self.test_name = "session"
        self.artifacts: Path | None = None
        self.runs = 0
        self.replaced: list[str] = []

    # -- commands

    def exec(self, cmd: str, timeout: float = 60) -> tuple[int, str]:
        """An AmigaDOS command whose output the agent collects; once more after the agent comes back from a stall."""
        try:
            return self.ami.exec_command(cmd, timeout=timeout)
        except (self.mod.AmigaUnreachable, OSError):
            self.wait_agent(180)
            try:
                return self.ami.exec_command(cmd, timeout=timeout)
            except (self.mod.AmigaUnreachable, OSError) as e:
                raise Unreachable(f"the agent is unreachable ({e})") from None

    def rc_of(self, text: str) -> int:
        """The return code a background rig implied: 0 for PASS, else the fail/error keys say 5, 10 or 20."""
        if re.search(r"^PASS\s*$", text, re.M):
            return 0
        return 20

    def _out_name(self, tool: str) -> str:
        """The next output file of this test: RAM:hw/out/<test>-<n>-<tool>.txt."""
        self.runs += 1
        safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", self.test_name)[:40]
        return f"{OUT}/{safe}-{self.runs}-{tool}.txt"

    def keep_output(self, remote: str, text: str) -> None:
        """A rig's output into the test's artifact dir."""
        if self.artifacts is not None:
            (self.artifacts / Path(remote.split("/")[-1]).name).write_text(text, encoding="latin-1")

    def log_settle(self, mark: int, tool: str) -> None:
        """Wait (3 s at most) for the rig's end mark in the debug log, so the test's slice is complete."""
        if self.capture is not None and self.capture.ok and tool in KV_TOOLS:
            self.capture.wait_for(rf"^\[{tool}\] end rc=", mark, 3.0)

    def command(self, tool: str) -> str:
        """How @tool is invoked: a rig from RAM:hw/bin (uploaded once), a tool by where it is installed, a C: command as it is."""
        if tool in TOOLS:
            remote = f"{BIN}/{tool}"
            self.upload_once(self.install_dir / TOOLS[tool], remote)
            return remote
        return PLACED.get(tool, tool)

    def run_text(self, tool: str, args: str, timeout: float = 120) -> tuple[int, str]:
        """A tool run to its end, its output redirected to a RAM: file and read back: (rc, text)."""
        out = self._out_name(tool)
        mark = self.capture.mark() if self.capture is not None else 0
        try:
            rc, _ = self.ami.exec_command(f"{self.command(tool)} >{out} {args}", timeout=timeout)
        except self.mod.AmigaError as e:
            if "still running" not in str(e):
                raise
            try:
                self.ami.break_command()
            except self.mod.AmigaError:
                pass
            self.reap()
            text = self.download_text(out)
            self.keep_output(out, text)
            raise ToolTimeout(f"{tool} still running after {timeout} s (broken):\n{text[-1200:]}") from None
        except (self.mod.AmigaUnreachable, OSError):
            self.wait_agent(180)  # the PI4's agent stalls now and then for a minute or two: once more
            try:
                rc, _ = self.ami.exec_command(f"{self.command(tool)} >{out} {args}", timeout=timeout)
            except (self.mod.AmigaUnreachable, OSError) as e:
                raise Unreachable(f"the agent is unreachable ({e})") from None
        text = self.download_text(out)
        self.log_settle(mark, tool)
        self.keep_output(out, text)
        return rc, text

    def run(self, tool: str, args: str, timeout: float = 120) -> Result:
        """A kv rig (VCTest, VPUPlay) run to its end, its output parsed."""
        rc, text = self.run_text(tool, args, timeout)
        return parse(text, rc)

    def start(self, tool: str, args: str) -> Job:
        """A tool started with Run >NIL:, its output in a RAM: file: a Job to wait for or stop.

        The shell's redirection holds the file exclusively until the tool ends (RAM: refuses every other open with
        "object in use"), so nothing can read a running job's output: done() reads the file once the tool is gone,
        and stop() finds the process by its command name.
        """
        out = self._out_name(tool)
        mark = self.capture.mark() if self.capture is not None else 0
        self.exec(f"Run >NIL: {self.command(tool)} >{out} {args}", timeout=15)
        return Job(self, tool, out, mark=mark)

    def reap(self) -> None:
        """Wait for a broken command to be reaped: the next EXEC is refused while it lives."""
        for _ in range(30):
            time.sleep(1)
            try:
                self.ami.exec_command("Echo >NIL: reaped", timeout=5)
                return
            except self.mod.AmigaError:
                continue

    def procs(self, name: str) -> list[int]:
        """The CLI numbers of the processes whose command is @name, by any path (Status FULL)."""
        rc, text = self.exec("Status FULL", timeout=15)
        found = []
        for num, command in re.findall(r"^Process\s+(\d+):.*Loaded as command: (.*)$", text, re.M):
            base = re.split(r"[/:]", command.strip())[-1]
            if base.lower() == name.lower():
                found.append(int(num))
        return found

    def break_all(self, name: str, timeout: float = 15) -> None:
        """Ctrl-C to every process of @name and wait for them to go."""
        for n in self.procs(name):
            self.exec(f"Break {n} C", timeout=10)
        t0 = time.time()
        while self.procs(name) and time.time() - t0 < timeout:
            time.sleep(1)

    # -- files

    def _retry(self, fn):
        """@fn once more after the agent answers again: a transfer that stalls once costs a retry, not the test."""
        try:
            return fn()
        except (self.mod.AmigaUnreachable, OSError):
            self.wait_agent(180)
            return fn()

    def download(self, remote: str) -> bytes:
        """A RAM: or system file, in 4 MiB pieces."""
        def once():
            data = bytearray()
            piece = 4 << 20
            while True:
                chunk = self.ami.read_range(remote, len(data), piece)
                data += chunk
                if len(chunk) < piece:
                    return bytes(data)
        return self._retry(once)

    def download_text(self, remote: str) -> str:
        """A text file's content, '' when it does not exist."""
        try:
            return self.download(remote).decode("latin-1", "replace")
        except self.mod.AmigaError:
            return ""

    def read_range(self, remote: str, offset: int, length: int) -> bytes:
        """@length bytes of a file from @offset."""
        return self._retry(lambda: self.ami.read_range(remote, offset, length))

    def upload_bytes(self, remote: str, data: bytes) -> None:
        """@data to @remote, in pieces when large, read back and compared."""
        if len(data) <= PIECE:
            self._retry(lambda: self.ami.write_file(remote, data))
        else:
            parts = [f"{remote}.part{i}" for i in range(0, len(data), PIECE)]
            for part, at in zip(parts, range(0, len(data), PIECE)):
                self._retry(lambda: self.ami.write_file(part, data[at:at + PIECE]))
            rc, text = self.exec(f"Join {' '.join(parts)} AS {remote}", timeout=60)
            self.exec(f"Delete >NIL: {' '.join(parts)} QUIET", timeout=30)
            if rc != 0:
                raise HarnessError(f"{remote}: Join failed ({rc}): {text.strip()}")
        if self.download(remote) != data:
            raise HarnessError(f"{remote}: read back differs")

    def upload(self, local: Path, remote: str) -> None:
        """A local file to the Amiga."""
        self.upload_bytes(remote, Path(local).read_bytes())
        self.ram_known.add(remote)

    def upload_once(self, local: Path, remote: str) -> None:
        """A local file to RAM:, unless it went up since the last boot."""
        if remote not in self.ram_known:
            self.upload(local, remote)

    def delete(self, *remotes: str) -> None:
        """Files on the Amiga, quietly."""
        if remotes:
            self.exec(f"Delete >NIL: {' '.join(remotes)} QUIET", timeout=30)
        for r in remotes:
            self.ram_known.discard(r)

    def version_of(self, remote: str) -> str:
        """The $VER string Version FULL reports for a file, '' when none."""
        rc, text = self.exec(f"Version {remote} FULL", timeout=20)
        return text.strip() if rc == 0 else ""

    def size_of(self, remote: str) -> int:
        """A file's size (List), -1 when missing."""
        rc, text = self.exec(f"List {remote} LFORMAT=%L", timeout=20)
        m = re.search(r"(\d+)", text) if rc == 0 else None
        return int(m.group(1)) if m else -1

    # -- the machine

    def ram_clear(self) -> None:
        """The material out of RAM:, to go up again on its next use; the rigs stay. A test of the RAM the VPU reaches asks for this."""
        material = sorted(r for r in self.ram_known if not r.startswith("RAM:hw/bin/"))
        for i in range(0, len(material), 8):
            self.delete(*material[i:i + 8])

    def ram_dirs(self) -> None:
        """RAM:hw with its bin and out drawers."""
        for d in (RAM, BIN, OUT):
            self.exec(f"MakeDir >NIL: {d}", timeout=15)

    def reboot(self) -> None:
        """C:Reboot (the 68k only), wait for the agent, forget RAM:."""
        mark = "RAM:hw-reboot-mark"
        self.ami.write_file(mark, b"1")
        try:
            rc, text = self.ami.exec_command("C:Reboot", timeout=3)
            raise HarnessError(f"C:Reboot returned ({rc}): {text.strip()}")  # a reboot never returns
        except HarnessError:
            raise
        except Exception:
            pass  # the agent goes down with the reboot
        t0 = time.time()
        while time.time() - t0 < 60:  # a reboot clears RAM: and the CM4 is back within a ping's timeout, so ask RAM:, not the agent
            try:
                self.ami.read_file(mark)
                time.sleep(1)
            except Exception:
                break
        else:
            raise HarnessError("C:Reboot: the Amiga never rebooted")
        self.wait_agent(240)
        time.sleep(10)  # Workbench and its startup
        self.ram_known.clear()
        self.ram_dirs()

    def wait_agent(self, timeout: float) -> None:
        """Until the agent answers a ping."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                self.ami.ping()
                return
            except Exception:
                time.sleep(3)
        raise Unreachable(f"the agent did not come back in {timeout} s")

    def wait_reset(self, timeout: float) -> bool:
        """Until someone resets the machine and the agent answers again, RAM: forgotten: TRUE when that happened.

        Only a reset empties RAM:. An agent that does not answer proves nothing: it may be stalled, as they do.
        """
        mark = "RAM:hw-reset-mark"
        self.ami.write_file(mark, b"1")
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                self.ami.read_file(mark)
            except self.mod.AmigaError:  # the agent answered, and the mark is gone
                time.sleep(10)  # Workbench and its startup
                self.ram_known.clear()
                self.ram_dirs()
                return True
            except (self.mod.AmigaUnreachable, OSError):
                pass
            time.sleep(2)
        return False

    def power(self) -> list[str]:
        """What the firmware saw of the Pi's power and temperature since the Pi started (get_throttled's lasting bits)."""
        _, text = self.exec("VCInfo GCMD get_throttled", timeout=15)
        word = re.search(r"throttled=(0x[0-9a-fA-F]+)", text)
        return [what for bit, what in POWER_EVENTS.items() if word and int(word.group(1), 16) & bit]

    def flush(self) -> None:
        """Avail FLUSH twice: expunge what nobody holds (a library of another build still in memory)."""
        self.exec("Avail >NIL: FLUSH", timeout=30)
        time.sleep(1)
        self.exec("Avail >NIL: FLUSH", timeout=30)

    # -- the screen

    def wake(self) -> None:
        """A nudge of the mouse: the Workbench's screen blanker (AmiKit's falling glyphs) closes its screen on input."""
        def nudge():
            self.ami.input_move_rel(1, 1)
            self.ami.input_move_rel(-1, -1)

        self._retry(nudge)
        time.sleep(0.3)

    def screenshot(self, rect: tuple[int, int, int, int] | None = None, screen: int = 0, name: str = "shot") -> np.ndarray:
        """A region of the frontmost screen as an RGB array, saved as a PNG in the test's artifact dir."""
        if rect:
            x, y, w, h = rect
            s = self.ami.screenshot(timeout=60, x=x, y=y, w=w, h=h, screen=screen)
        else:
            s = self.ami.screenshot(timeout=60, screen=screen)
        if self.artifacts is not None:
            (self.artifacts / f"{name}.png").write_bytes(self.png.screenshot_png(s))
        if s.palette is not None:
            pal = np.array(s.palette, np.uint8)
            return pal[np.frombuffer(s.pixels, np.uint8)].reshape(s.height, s.width, 3)
        return np.frombuffer(s.pixels, np.uint8).reshape(s.height, s.width, 3)

    def window(self, title: str) -> tuple[int, int, int, int]:
        """The frontmost screen's window titled @title: (x, y, w, h); the blanker woken first."""
        self.wake()
        for line in self.ami.ui_tree().splitlines():
            m = re.match(r'W\s+\d+\s+(-?\d+)\s+(-?\d+)\s+(\d+)x(\d+)\s+\S+\s+"(.*)"', line)
            if m and m.group(5) == title:
                return tuple(int(m.group(i)) for i in range(1, 5))  # type: ignore[return-value]
        raise AssertionError(f"no window \"{title}\" on the frontmost screen")

    def click(self, x: int, y: int) -> None:
        """A left click at screen coordinates."""
        self.ami.input_click(x, y)
