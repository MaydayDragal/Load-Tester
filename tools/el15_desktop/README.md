# EL15 Load Control — desktop

A PC front end for the ALIENTEK EL15 electronic load, over the same BLE protocol
the phone app speaks. Live monitoring, manual control, and the
circuit-resistance sweep.

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
whether the link works without touching the UI or the load.

## Not included

The **battery capacity test** — hours-long discharges with cutoff, rest and
state-of-charge tracking. The engine exists on the phone and the board; this app
has the transport and the report plumbing it would need, so it is the obvious
next addition rather than a gap in the design.
