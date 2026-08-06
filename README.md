# EL15 Load Control

Two independent controllers for an **ALIENTEK EL15** electronic load (a DC "load
tester") over Bluetooth Low Energy. Either one is the BLE central and drives the
load on its own — they are alternatives, not a pair, and neither needs the other.

| Directory | What it is |
|---|---|
| [`firmware/`](firmware/) | **Standalone ESP32-C6 controller** — a Waveshare ESP32-C6-Touch-AMOLED-1.8 with a touchscreen instrument UI, resistance-sweep and battery-capacity engines, SD-card CSV reports and a stack of safety supervisors. No PC and no phone. **Hardware-verified**, including an 8.9 h unattended capacity discharge |
| [`app/`](app/) | **Android app** — the same job from a phone: same protocol, same engines, same report format. **Not yet run against a real EL15** |

The BLE protocol re-implements the one used by the desktop
[DM40GUI](https://github.com/maj113/DM40GUI) project (see its
[Device Support](https://github.com/maj113/DM40GUI/blob/master/README.md#device-support)
section — the EL15 is the electronic load), plus the trailing command checksum a
real EL15 requires, found by bench testing on 2026-07-24.

## Which one to use

The **board** is the verified one, and the one to trust for a long unattended
run: it has done a real 8.9 h discharge and stopped itself at the cutoff. The
**app** needs no hardware beyond the phone you already have, and is the easier
place to start — but it has never driven a real load, so its first run deserves
[`firmware/FIRST_CONTACT.md`](firmware/FIRST_CONTACT.md) treatment.

Both write the **same CSV report layout**, so results from either drop into the
same spreadsheet.

## Getting the app

CI builds an installable APK on every push:
**Actions → Build APK → `el15-load-control-debug`**. Download it on the phone,
unzip, install. Details in [`app/README.md`](app/README.md).

## Docs

| Doc | What it is |
|---|---|
| [`app/README.md`](app/README.md) | Android app: features, what a phone changes, architecture, build |
| [`firmware/README.md`](firmware/README.md) | Firmware: features, architecture, build/flash |
| [`firmware/HANDOVER.md`](firmware/HANDOVER.md) | Living session handover — current state, gotchas, next work |
| [`firmware/QA_GUIDE.md`](firmware/QA_GUIDE.md) | How to QA it: test matrix, wire protocol, safety behavior |
| [`firmware/FIRST_CONTACT.md`](firmware/FIRST_CONTACT.md) | Safe bench order for a real EL15 |
| [`firmware/QA_REPORT.md`](firmware/QA_REPORT.md) | Dated code-audit snapshots + resolutions |
| [`firmware/CAPACITY_PLAN.md`](firmware/CAPACITY_PLAN.md) | Battery-test roadmap |
| [`firmware/RTEST_ACCURACY.md`](firmware/RTEST_ACCURACY.md) | R-test measurement methodology |

> **History note:** the app and an Android BLE **load simulator** were removed on
> 2026-08-03 to slim the repo to the firmware alone. The app came back on
> 2026-08-06 with its command-checksum defect fixed and the firmware's newer
> engines ported across. The simulator is still only in git history, last present
> at commit `1cd5607` — check it out from there to bench-test the firmware with a
> second phone and no load hardware.

> ⚠ **Safety** — this drives real current. Set the EL15's own hardware UVP as a
> backstop before discharging a pack: a controller's link guard needs a working
> radio to act, the instrument's UVP does not.
