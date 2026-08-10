# EL15 Load Control — Android app

Turns an Android phone into a controller for the ALIENTEK **EL15** electronic
load over Bluetooth Low Energy — the same role the ESP32-C6 board plays in
[`firmware/`](../firmware/), with the phone as the BLE central. The board is not
involved; the app talks straight to the load.

> **Status (2026-08-06): builds and passes its tests; not yet run against a real
> EL15.** This app was in the repo until 2026-08-03, when it was removed to slim
> the tree to the firmware alone. It is back, with its one known defect fixed
> (see [Protocol](#protocol)) and with the test engines the firmware has grown
> since. Nothing here has been bench-verified on hardware — the firmware has,
> extensively, and this is a faithful port of it, but a port is not a
> measurement. Treat the first run against a real load as first contact and
> follow [`firmware/FIRST_CONTACT.md`](../firmware/FIRST_CONTACT.md).

---

## Feature set

Parity with the firmware, adapted where a phone differs from the board.

| Area | What it does |
|---|---|
| **Connect** | Scan (named devices only, deduped), connect, disconnect. Reconnect is scan-based so **random-address** peers work — a plain address reconnect assumes a public address and silently fails against them |
| **Monitor** | Arc gauges for V and I, power bar, mode, telemetry line (runtime · temp · fan · Ah/mΩ), live waveform, raw packet inspector with CRC |
| **Adjust** | Mode chips CC · CV · CR · CP · CAP · DCR, unit-aware setpoint entry, LOAD ON/OFF, keypad lock |
| **R-Test** | Fuse-aware **continuous triangular current sweep**. Every status packet is fitted by a running least-squares regression, so the result carries a real **± uncertainty**; reports series resistance, Voc, R², estimated short-circuit current, sag, peak power, temperature range. 2-wire/4-wire with a shorted-probe **tare** |
| **Battery** | Ten chemistry presets each with its own OCV curve and 60 V cell ceiling, cell-count suggestion from the connected pack, C-rate chips, CC discharge with local Ah/Wh integration, debounced cutoff, pause/resume, rest/rebound, pack internal resistance from the switch-on sag, and a **time-to-cutoff estimate read off the discharge curve** with the pack's capacity learned during the run |
| **Reports** | `RTEST_NNN.CSV` / `BATT_NNN.CSV` into `Downloads/EL15 Load Control`, matching what the board writes to its SD card field for field — plus a printable `RTEST_NNN.pdf` / `BATT_NNN.pdf` under the same number, drawn as a real document rather than a screenshot |
| **Probe wiring** | Global 2-wire / 4-wire (Kelvin). In 2-wire the lead resistance is added back to every reading — monitor, graph, battery cutoff and reports alike — so measurements belong to the part and not to your leads |
| **Safety** | On-screen emergency stop in the header · link-loss reconnect-and-kill supervisor · crash recovery across a process death · load-safe disconnect · engine mutual exclusion · foreground service so a long run survives the UI being destroyed |
| **Settings** | Theme · poll interval · probe wiring · sweep and battery defaults · alarms · periodic CSV snapshot · demo circuit |

### Deliberately not included

Scope is firmware parity, so the extras this app used to carry are gone: test
**History**, **Trends**, the home-screen **widget**, and the quick-settings
**tile**. So are the **runtime / step / OCP** bench engines and the
**calibration sweep**, which the firmware README lists as never ported. All of
it remains in git history at `1cd5607`.

**PDF export came back** (2026-08-06) and is the one place this app now does
more than the board: the board has no printer and no document to hand someone.
It reports the same numbers as the CSV, so nothing is reachable only by reading
a chart.

The in-process **demo simulator** is kept, as a development aid — it is the only
way to see the app work before an EL15 is wired up. The firmware deliberately
has none; nothing the simulator produces is a measurement.

---

## What the phone changes

| Firmware | Here | Why |
|---|---|---|
| BOOT-button hardware e-stop | Header **⏻** button, never disabled | No hardware button. It fires immediately when anything is energised — a confirm dialog in front of an e-stop defeats the point |
| SD-card CSV report | `Downloads/EL15 Load Control` via MediaStore | Same file layout, so a phone-saved run and a board-saved run drop into the same spreadsheet |
| Datapoint log in internal flash | The same tiered log, in RAM | A phone has the memory; the tier schedule is kept anyway so CSV sizes and curve shapes match the board's |
| ES8311 codec tones | `ToneGenerator` | — |
| NVS in-flight flag | `SharedPreferences.commit()` | `apply()` flushes on a background thread, so a process kill loses exactly the flag whose purpose is surviving one |
| AMOLED burn-in shift/dim | Keep-screen-on setting | Not the phone's problem |
| RTC "not set" fallback | Always a real timestamp | A phone's clock is set |
| PMIC brownout auto-off | — | No equivalent; the link guard and the load's own UVP are the backstops |
| Sweep fits every valid packet | Fits every packet the load actually held the commanded current for | The one MEASUREMENT difference between the two engines. At ~1.2 A the EL15 emits one or two frames per sweep whose current field is wrong while the voltage stays on the fitted line; unfitted they inflate the reported uncertainty 2.6×. See `RTEST_ACCURACY.md` §7 — the firmware does not have the gate until it can be run on the board |

**Poll rate.** The sweep fits every packet that arrives, so the poll interval is
what tightens the resistance uncertainty. The default is **50 ms (20 Hz)**,
matching the firmware, and the app requests `CONNECTION_PRIORITY_HIGH` on
connect — at Android's default connection interval a 50 ms poll cannot be
serviced. Whether the peer grants it is up to the peer.

---

## Protocol

Command frames are `AF 07 03 <cmd> <len> <data...> <checksum>`, where the
checksum byte makes the whole frame sum to **0 mod 256** — the same rule the
status parser enforces on incoming packets. A real EL15 silently drops any
command whose bytes don't sum to zero.

> **The defect this app shipped with, now fixed.** Its command constants were
> hand-written as prefix+value with **no checksum byte**, inherited from the
> DM40GUI reference. POLL worked (its `0x3F` was captured whole), so telemetry
> flowed and the app looked healthy — while every mode, setpoint and LOAD write
> was dropped by real hardware. Found by bench testing on 2026-07-24 and fixed
> in the firmware the same day; this is that fix ported back. Frames are now
> *built* through `withChecksum()` rather than hand-written, and
> `ChecksumTest`/`El15ProtocolTest` pin the bytes. The old test assertions
> encoded the broken frames and passed happily, which is why they are called out
> in the test file.

Status packets arrive as 28-byte notifications on `FFF1`; commands are written
to `FFF3`; the service is `FFF0`. Full wire details:
[`firmware/QA_GUIDE.md`](../firmware/QA_GUIDE.md).

---

## Architecture

```
DeviceCore        process-lifetime hub — owns transport, engines, guard;
                  routes status packets. The app's main.cpp
El15Protocol      wire protocol (pure): frames, checksum, status parsing
El15BleManager    BLE central: scan/connect/MTU/notify/reassembly, serialized
                  GATT op queue, emergency-off that jumps the queue
El15Controller    the interface the engines drive (never BLE directly)
ResistanceTest    continuous triangular sweep + running least squares
LeastSquares      the fit, pure and unit-tested on its own
CapacityTest      discharge engine + pack IR + charge-state ETA
BatteryModel      chemistry OCV curves and standard C-rates (pure)
LinkGuard         link-loss reconnect-and-kill + crash recovery
SampleLog         tiered datapoint log feeding the report's per-sample block
Report            RTEST_/BATT_ CSV, matching the board's layout
MonitorService    foreground service so a long run outlives the Activity
MainActivity      instrument panel; a view over the core, freely destroyable
ResultActivity    result screen for both engines
```

**The one routing rule not to break:** `DeviceCore.onStatus` applies probe
compensation once, then hands the **corrected** packet to the UI and the
capacity engine but the **raw** packet to the R-Test. The sweep's fit already
removes the lead tare from the slope; a pre-corrected voltage would subtract the
same resistance twice and a low-milliohm result could land at zero.

Everything is main-thread confined — the transports already deliver there, and
the engines are pumped by `Handler` tickers.

---

## Build

CI builds it on every push: **Actions → Build APK → `el15-load-control-debug`**.
Download that artifact on the phone, unzip, install (allow "install unknown
apps" for your browser or file manager once).

Locally, with a JDK 17 and the Android SDK:

```bash
./gradlew testDebugUnitTest    # 50 unit tests, no device needed
./gradlew assembleDebug        # app/build/outputs/apk/debug/app-debug.apk
```

`assembleRelease` is debug-signed on purpose so it is installable by
sideloading; replace the signing config before any store release.

---

## Using it

1. **Scan & connect** → pick your EL15 (or the demo device to look around).
2. **Monitor** shows the live readout; the mode chips and setpoint drive the
   load; **LOAD** toggles it.
3. **R-TEST** runs the sweep. If you are 2-wire, run **Measure lead resistance**
   once with the probes shorted together first — at the milliohm scale that
   correction is usually larger than what you are measuring.
4. **BATT** runs a capacity discharge. Pick the chemistry, confirm the cell
   count, set a current (or tap a C-rate chip), check the cutoff.
5. Both offer **Save report** on their result screen.
6. The header **⏻** is an emergency stop from any screen.

> ⚠ **Safety** — this drives real current. Set the EL15's own hardware UVP as a
> backstop before discharging a pack: the app's link guard needs a working radio
> to act, the instrument's UVP does not. Keep the phone on a charger for long
> unattended runs, and stay clear of the setup while a test runs.
