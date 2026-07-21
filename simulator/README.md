# EL15 Load Simulator (Android)

A standalone Android app that **pretends to be an EL15 electronic load over
Bluetooth**. It runs a real BLE GATT peripheral — advertises the FFF0 service,
accepts command writes on FFF3, and pushes 28-byte status notifications on
FFF1 — using the exact wire format of the real device.

Use it to test the **ESP32 controller** (or the phone app) against a genuine
Bluetooth signal, with no load hardware on the bench.

## Why a separate app

The main app has an in-process `El15Simulator` for its own demo mode, but that
never goes on the air. This app is the *peripheral* side of the protocol: it
**decodes** the commands the central sends and **encodes** status packets back,
so from the ESP's radio it is indistinguishable from a real EL15.

## How to use

1. Install on a second Android phone (one that can act as a BLE peripheral —
   most modern phones can; the app warns if yours can't).
2. Set the **virtual circuit**: source voltage (emf) and series resistance. A
   resistance test run against it will recover these values.
3. Optionally set the **advertised name** (default `EL15-SIM`).
4. Tap **Start advertising**. Grant the Bluetooth permission if asked.
5. On the ESP32 (or the phone app), scan and connect — the simulator appears by
   its advertised name.
6. Watch the **live state** (V / I / P, mode, setpoint, load, lock, protection)
   and the **command log** update as the controller drives it. Turning the load
   on, sweeping current in the resistance test, over-power/over-current trips —
   all behave like the real unit.

## Behaviour modelled

- All device modes: CC / CV / CR / CP / CAP / DCR (setpoint honoured per mode).
- Terminal voltage sags by the IR drop under load; CAP integrates Ah/Wh;
  DCR reports milliohms.
- EL15 ratings enforced: 12 A / 150 W / 60 V, with OPP/OCP protection frames.
- Fan speed and temperature scale with current, as on hardware.
- Responds to each poll with a fresh frame and pushes on a timer so the
  accumulators keep advancing while connected.

## Notes

- Same package family as the main app but a distinct `applicationId`
  (`com.loadtester.el15sim`), so both install side by side.
- The wire format is covered by unit tests (`LoadModelTest`) — decoding the
  fixed command frames and encoding header-valid, zero-checksum status packets.
- BLE notifications carry the full 28-byte frame in one packet once the central
  negotiates an ATT MTU ≥ 31. The ESP firmware requests MTU 247 on connect; the
  phone app negotiates MTU too. (If you point a central that stays at the
  23-byte default at it, the frame will be split — the ESP's reassembly handles
  that, but a larger MTU is preferred.)
