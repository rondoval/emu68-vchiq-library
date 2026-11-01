# The hardware regression suite

pytest tests that drive one Amiga through amimcp, with the rigs `VCTest` and
`VPUPlay` on the Amiga printing facts and the host judging them. The full
account is developer-guide.md section 8; this is the short form.

## Once

    python3 -m venv tests/.venv && tests/.venv/bin/pip install -r tests/requirements.txt
    tests/.venv/bin/python tests/harness/material.py     # the material ahead of time (optional)

The material is made on the host from one sample video that the first run
downloads from convertico.com into `tests/.cache/source/` and checks by its
SHA-256 (developer-guide.md section 8.8); offline, put the file there by hand.

[amimcp](https://github.com/thomas-luebker/amimcp) is checked out beside the
stack, or named with `--amimcp`. The Amiga's address and token come from its
entry in `~/.claude.json` (`--server amiga-pi4` by default), or from
`AMIGA_HOST` and `AMIGA_TOKEN`.
The stack must be built (debug flavour, test hooks on) with its `install/`
and `build/` in place: the suite replaces `DEVS:vchiq.device`,
`LIBS:videocodec.library`, `LIBS:videocodec/videocore.codec`, `C:VCInfo`,
`SYS:Tools/Commodities/VCGrab` and its icon, `DEVS:AHI/videocore.audio`,
`DEVS:AudioModes/VIDEOCORE`, `SYS:Classes/DataTypes/vcjpeg.datatype` and
`SYS:Storage/DataTypes/VCJPEG`
from the install tree and reboots; nothing else on the Amiga changes, and
everything temporary goes to `RAM:hw`. The Amiga must have the project
installed by the installer (the datatype's line in `S:User-Startup`
included).

## Running

    tests/.venv/bin/pytest tests -m smoke        # one of every binary and path, about six minutes
    tests/.venv/bin/pytest tests                 # everything automatic, about 25 minutes
    tests/.venv/bin/pytest tests -m "not slow"   # without the level probes and the whole still set
    tests/.venv/bin/pytest tests --collect-only -q
    tests/.venv/bin/pytest tests/unit            # the harness's own host-only tests

Options: `--server`, `--install-dir`, `--build-dir`, `--run-dir`,
`--no-install`, `--no-reboot`, `--listen`, `--hands`, `--profile`,
`--serial DEV|none`, `--require-debug-log`, `--update-baselines`,
`--keep-ram`; `pytest --help` lists them under "the machine under test".

Markers: `smoke`, `slow`, `reboot` (run last; `--no-reboot` drops them),
`hooks` and `profile` (skipped or deselected unless the build allows),
`listen` and `hands` (only with `--listen`, `--hands`: someone at the machine
answers in the terminal).

## What comes out

`tests/runs/<timestamp>/`: `report.md`, `run.json`, `serial.log`,
`measured.json`, and one directory per test with the rigs' outputs, fetched
pictures, screenshots and that test's slice of the debug log. `-ra` on the
command line (the default) lists every failure and skip with its reason.

## Baselines

`baselines.json` holds measured numbers per machine. A test says what a
number means where it checks it:

    baselines.check("fullscreen.play.fps", r.num("play.fps"), higher=True, tolerance="10%")

No baseline yet: recorded with a warning. `--update-baselines` writes this
machine's measured values into the file at the end; read the diff before
committing it.

## The grammar

`VCTest` and `VPUPlay` print `format=1`, then `key=value` lines (groups by
number: `stream.0.width`), prose indented two spaces, and `PASS` or `FAIL`
last; return codes 0 passed, 5 a self-check failed, 10 the environment, 20
the API or the arguments. `harness/results.py` parses it; the keys are in
the rigs' sources (`tools/vctest/`, `tools/VPUPlay.c`) and developer-guide
section 6.

## Layout

    conftest.py        options, fixtures, ordering, the report
    harness/amiga.py   the device over amimcp: run, start, upload, download, reboot, screenshot
    harness/serial.py  the debug log capture and its slices
    harness/results.py the grammar
    harness/material.py, stills.py   the material recipes and cache
    harness/pictures.py  PSNR, PPM, the colour bars
    harness/baselines.py, report.py, perf.py, tools.py
    test_*.py          the tests, one module per area
    unit/              host-only tests of the harness
