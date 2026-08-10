# EL15 Load Control — desktop

A PC front end for the ALIENTEK EL15 electronic load, over the same BLE protocol
the phone app speaks. Live monitoring, manual control, and the
circuit-resistance sweep.

## Just run it

Download **`EL15 Load Control.exe`** from the `el15-load-control-windows`
artifact of the *Build desktop app* workflow, or build it yourself with
`python build_exe.py`. Then double-click it. One file, nothing to install — no
Python, no libraries.

The first launch may show *"Windows protected your PC"*, because the executable
is not code-signed. **More info → Run anyway.** Signing it needs a certificate;
until there is one, that prompt is expected.

Then: **Scan** → **Connect**. Bluetooth must be on.

## Or run it from source

```
pip install bleak
python el15_desktop.py
```

Tkinter ships with CPython, so `bleak` is the only dependency. The address
defaults to the unit this was written against — override with
`EL15_ADDR=AA:BB:...`, or press **Scan**.

## What it does

| | |
|---|---|
| **Monitor** | V / I / P read large, plus mode, temperature, fan, setpoint and load state; a rolling V and I chart; the raw 28-byte frame; CSV export of the recorded window |
| **Control** | mode (CC · CV · CR · CP · CAP · DCR) with the right unit and precision per mode, setpoint entry, LOAD ON/OFF |
| **R-Test** | fuse-aware triangular sweep with live progress, then R ± σ, Voc, R², reliability, the measured current range, sag, peak power, temperature range, and the V–I curve with its fitted line. Saves a CSV carrying the banded curve, the result block and every raw packet |
| **Safety** | an emergency stop that is always enabled and never asks; load off on disconnect, on closing the window, on a BLE error, and on any test ending however it ends |

## It is a front end, not a second implementation

The protocol client and the sweep engine are imported from
[`../el15_bench`](../el15_bench) — the code validated against real hardware:
command checksums, control-write pacing, frame reassembly, the off-target gate,
the spike test and the corruption repair. A GUI that reimplemented any of that
would drift from the phone app and the board, and the point of this project is
that the three agree.

The one thing this file does implement itself is the sweep's own loop, because
it has to report progress to a UI. That is why `--rtest-check` exists.

## Checking it

```
python el15_desktop.py --selftest      # build the UI, lay it out, render a result, exit
python el15_desktop.py --check         # drive the real load, no UI, no current drawn
python el15_desktop.py --rtest-check   # run the app's own sweep on hardware - DRAWS CURRENT
```

`--check` is the useful one when something is wrong: it exercises the
asyncio-thread bridge, the session lifecycle and the teardown, and tells you
whether the link works without touching the UI or the load. All three also write
`el15_check.log` beside the program, because a windowed build has no console to
print to.

Two things about the packaged build that will waste your time otherwise:

- **onefile runs the app as a CHILD of the bootloader process.** Killing "the"
  process in Task Manager can leave the real app running, and inspecting the
  parent's windows finds only `PyInstaller Onefile Hidden Window` — which looks
  exactly like a GUI that failed to open, and is not.
- **Force-killing it skips the load-off teardown.** Closing the window runs it;
  `taskkill` does not. Use the window, or the emergency stop.

## Not included

The **battery capacity test** — hours-long discharges with cutoff, rest and
state-of-charge tracking. The engine exists on the phone and the board; this app
has the transport and the report plumbing it would need, so it is the obvious
next addition rather than a gap in the design.
