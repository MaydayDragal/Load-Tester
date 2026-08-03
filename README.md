# EL15 Load Control — standalone ESP32-C6 controller

Firmware that turns a **Waveshare ESP32-C6-Touch-AMOLED-1.8** into a standalone
controller for an **ALIENTEK EL15 electronic load** (a DC "load tester") over
Bluetooth Low Energy: BLE central, touchscreen instrument UI, resistance-sweep
and battery-capacity test engines, SD-card CSV reports, and a stack of safety
supervisors. No PC and no phone required.

The BLE protocol re-implements the one used by the desktop
[DM40GUI](https://github.com/maj113/DM40GUI) project (see its
[Device Support](https://github.com/maj113/DM40GUI/blob/master/README.md#device-support)
section — the EL15 is the electronic load), plus the trailing command checksum a
real EL15 requires, found by bench testing on 2026-07-24.

Everything lives in [`firmware/`](firmware/) — start with
[`firmware/README.md`](firmware/README.md). The docs that matter:

| Doc | What it is |
|---|---|
| [`firmware/README.md`](firmware/README.md) | Features, architecture, build/flash |
| [`firmware/HANDOVER.md`](firmware/HANDOVER.md) | Living session handover — current state, gotchas, next work |
| [`firmware/QA_GUIDE.md`](firmware/QA_GUIDE.md) | How to QA it: test matrix, wire protocol, safety behavior |
| [`firmware/FIRST_CONTACT.md`](firmware/FIRST_CONTACT.md) | Safe bench order for a real EL15 |
| [`firmware/QA_REPORT.md`](firmware/QA_REPORT.md) | Dated code-audit snapshots + resolutions |
| [`firmware/CAPACITY_PLAN.md`](firmware/CAPACITY_PLAN.md) | Battery-test roadmap |

> **History note:** this repo previously also carried an Android control app
> (`app/`) and an Android BLE load simulator (`simulator/`, used to bench-test
> the firmware with no load hardware). Both were removed on 2026-08-03 to slim
> the repo to the firmware alone — they remain in git history, last present at
> commit `1cd5607`. The app never received the command-checksum fix, so it could
> read a real EL15 but not drive one.
