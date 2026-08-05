# EL15 Controller Firmware — QA Guide

Advanced-QA reference for the **standalone ESP32-C6 firmware** in `firmware/`
(the on-device controller for the ALIENTEK EL15 electronic load). Covers current
status, how to build/flash/drive it, the code layout, a per-feature test matrix,
the wire protocol, safety-critical behavior, and known gaps/risks.

> Scope: the **`firmware/` ESP32-C6 target** running the v2 "Focus" touchscreen
> UI. The Android app and the phone-based BLE load simulator this guide once
> referenced as test tools were removed from the repo on 2026-08-03 (last
> present at `1cd5607`) — check the simulator out from git history for
> hardware-free testing, or test against the real EL15.

**Last updated:** 2026-08-05. Companion docs:
[`HANDOVER.md`](HANDOVER.md) (state + gotchas), [`FIRST_CONTACT.md`](FIRST_CONTACT.md)
(real-EL15 bench order), [`QA_REPORT.md`](QA_REPORT.md) (audit + resolutions).

---

## 1. Current status (what's verified vs. not)

| Area | State |
|---|---|
| Build (PlatformIO, pioarduino, ESP32-C6) | ✅ clean under `-Wall -Wextra`; ~2.19 MB of the 3 MB `huge_app` slot, RAM 19.8 % static |
| Boot on hardware | ✅ clean, no panic/bootloop; `[boot] reset reason:` printed every boot |
| Display (CO5300/SH8601 AMOLED, QSPI 80 MHz) | ✅ renders the v2 UI, correct colors |
| Touch (CST820 answering as FocalTech) | ✅ works; 10 ms sampling + touch-snap engine |
| Audio (ES8311) | ✅ `[audio] ready (ES8311)`; tones clean since the continuous-stream fix |
| BLE connect to **phone simulator** (random address) | ✅ connects and streams |
| BLE connect to **real ALIENTEK EL15** | ✅ verified 2026-07-24 |
| Live telemetry from a real EL15 | ✅ verified — poll-driven, ~17–19 fresh samples/s |
| Mode / setpoint / LOAD ON-OFF on a real EL15 | ✅ verified — **after** the command-checksum fix (§7) |
| SD card read + write, FAT timestamps, re-init after eject | ✅ verified on hardware via `EL15_SDTEST` (2026-08-01) |
| Flash datapoint log (LittleFS mount, tier schedule, replay) | ⚠️ verified on hardware 2026-08-01, then **reworked 2026-08-05** — ms timestamps and a second `rtest` instance so every sweep packet is logged (`sample_log.h`). `EL15_SDTEST` exercises both logs; needs a re-run |
| Mode commands actually TAKING on a real EL15 | ✅ since 2026-08-05 — measured 2 of 9 silently dropped before the confirm-and-retry, 0 of 8 after (HANDOVER §18) |
| SD **UI Save buttons** / auto-save on completion | ⚠️ auto-save has run for real (`BATT_013`), but **both real saves produced a corrupt file** — a card-level fault, and the verification that should have caught it had four holes (HANDOVER §17). Fixed; **not yet re-run against a good card** |
| **Verified save** (CRC-32 read-back, cache-defeating) | ⚠️ rebuilt 2026-08-05, never yet run against a card that behaves |
| Capacity run with **real current** | ✅ **`BATT_013`, 2026-08-05** — 92 Ah lead-acid, 10.0 A, 8.9 h unattended, 88.58 Ah / SoH 96.3 %, self-stopped at the 8.00 V cutoff |
| R-Test with **real current** | ✅ verified 2026-08-01 — continuous sweep, repeats within 0.18–0.9 % |
| Rated-capacity metrics, C-rate, SoH, charge-state model | ✅ all exercised by `BATT_013` and agreeing with each other |
| Derived safety caps (capacity / duration) | ✅ armed correctly at 184 Ah / 23 h and stayed out of the way |
| Pause/resume, running-test chip | ⚠️ compile-clean, needs a real run |
| Link-guard hot drop, crash recovery, brownout auto-off | ⚠️ the guard has now fired — but only **spuriously**, on the failed-start cascade since fixed (HANDOVER §18). Never triggered by a genuine unattended drop |
| NVS persistence / burn-in / NTP / Kelvin+tare | ⚠️ compile-clean, not confirmed end-to-end |

**Top things QA should confirm first:** (1) a full **Save to a NEW SD card** —
the verification was rebuilt on 2026-08-05 and has never run against a card that
behaves; (2) **a capacity start**, to confirm the priming fix (watch for
`[batt] priming got NO status packets`); (3) the link-guard hot-drop drill.
`FIRST_CONTACT.md` sequences all of this safely.

---

## 2. Hardware & the two test surfaces

**Board:** Waveshare **ESP32-C6-Touch-AMOLED-1.8** — 1.8″ AMOLED **368 × 448 px
portrait**, CO5300 controller driven as SH8601 (QSPI), CST820 touch answering on
the FocalTech register map (I²C), TCA9554 I/O expander (panel + speaker-amp
enable), AXP2101 PMIC, PCF85063 RTC, ES8311 audio codec, microSD slot. RISC-V,
Wi-Fi 6 / BLE 5, 16 MB flash, **no PSRAM**.

The COM port **hops between resets** (COM4 and COM7 both seen) — discover it,
don't hard-code it.

There are **two** test surfaces (the on-device demo simulator was removed —
bench-testing always goes over a real BLE link now):

1. **Phone BLE simulator** (real BLE path, no load hardware; removed from the
   tree 2026-08-03 — check out from git history at `1cd5607` and build it).
   Advertises the EL15 GATT service as a peripheral and models either a fixed
   circuit or a full battery with a chemistry-accurate discharge curve. **Random
   address** — exercises the scan/connect path that RPA peers need.
2. **Real ALIENTEK EL15.** Connect, telemetry, control, a full R-test sweep and
   a full 8.9 h capacity discharge are all verified against it. See
   `FIRST_CONTACT.md` for the safe bench order.

---

## 3. Build / flash / monitor

PlatformIO Core is installed **off-PATH** at
`~/.platformio/penv/Scripts/pio.exe`. There is no `idf.py`/`esptool` on PATH.

```bash
PIO=~/.platformio/penv/Scripts/pio.exe
PORT=$("$PIO" device list | grep -oE 'COM[0-9]+' | head -1)
"$PIO" run   -d firmware                                  # build
"$PIO" run   -d firmware -t upload --upload-port "$PORT"  # flash
"$PIO" device monitor -p "$PORT" -b 115200                # serial log
```

- First build downloads ~1 GB (platform + RISC-V toolchain + arduino-esp32 3.1.3
  + libs); later builds are incremental (~15 s for `src/`). Changing
  `include/lv_conf.h` forces a full LVGL recompile (~75–125 s).
- Serial is 115200. Useful lines: `[boot] reset reason: …` (power-on / PANIC /
  BROWNOUT / WDT / USB), `[audio] ready (ES8311)`, a per-connect **GATT
  characteristics dump**, `[ble] write OK/FAIL …` per control write, a 1 Hz
  `[ble] status rx: …`, `[ble] status frame DROPPED (checksum)`, `[guard] …`,
  `[pmic] …`, `[btn] EMERGENCY STOP`, `[sd] wrote …`, `[batt] done:/error:`.
- `esp_core_dump_flash: Incorrect size of core dump image` at boot is
  **harmless** (no coredump partition). Occasional `Wire.cpp requestFrom Error -1`
  are benign transient touch-I²C hiccups.
- **If the board vanishes from USB entirely** (no COM port at all — seen once
  under a very fast poll flood), physically unplug/replug the cable.

Library versions are pinned **exactly** in `platformio.ini` to what the
hardware-verified image was bench-tested against: LVGL **8.4.0**, GFX **1.6.7**,
NimBLE **2.5.0**, SdFat **2.3.1**. They were `^` ranges until 2026-08-03, which
let a clean checkout resolve libraries no verified image had ever run. Bump them
deliberately, re-verify on hardware, then update this line.

**Test scaffolding** (compiled out of normal builds):
`-D EL15_SDTEST` (boot-time SD write/readback + FAT stamp + LittleFS log check),
`-D EL15_RTUNE` (R-test tuning matrix — **draws current**; waits for a key press),
`-D EL15_POLLTEST` (poll-rate sweep), `-D EL15_SELFTEST` (safe mode-cycle sweep,
load stays OFF), `-D EL15_NO_POLL` (disable polling — proves telemetry is
poll-driven). E.g.
`PLATFORMIO_BUILD_FLAGS="-D EL15_SDTEST" "$PIO" run -d firmware -t upload …`.
**Clear `PLATFORMIO_BUILD_FLAGS` before building a shippable image** — it is an
env var and persists silently across commands in the same shell.

> ⚠ **Opening a serial monitor RESETS this board.** The USB-Serial/JTAG
> peripheral reads DTR/RTS transitions as a reset request. Never attach one while
> the load may be energised — doing so mid-sweep once rebooted the controller
> with ~1 A flowing and left the EL15 sinking until its own protection tripped.
> To read serial without resetting, open the port with DTR and RTS both false and
> do not pulse RTS. See HANDOVER §1.

---

## 4. Code layout & architecture

```
firmware/src/
  main.cpp            owns objects, routes events, buttons, e-stop, power monitor
  el15_protocol.h     wire protocol: parse + command frames (header-only, pure)
  el15_client.{h,cpp} BLE central: scan/connect/subscribe/poll/reassemble/pace
  el15_controller.h   El15Controller interface ONLY (no on-device simulator)
  resistance_test.h   fuse-aware bidirectional sweep engine + slope uncertainty
  capacity_test.h     battery discharge / capacity engine (+ SoC-based ETA)
  battery_model.h     chemistry OCV-vs-charge curves + standard test C-rates
  display.{h,cpp}     panel + touch + LVGL + PMIC/RTC + buttons + sleep + burn-in
  audio.{h,cpp}       ES8311 tone feedback (continuous-stream I2S task, prio 8)
  es8311.{c,h}        vendored codec driver
  sd_card.{h,cpp}     microSD on bit-banged software SPI (SdFat, custom driver)
  report.h            RTEST_/BATT_ CSV bodies
  prefs.{h,cpp}       NVS persistence (debounced) + synchronous safety flags
  link_guard.h        link-loss auto-stop supervisor + crash recovery
  netclock.{h,cpp}    Wi-Fi scan + NTP → PCF85063
  ui.{h,cpp}          v2 "Focus" LVGL UI (~3100 lines — the bulk of the app)
  board_config.h      ALL board GPIO/pins (verified vs Waveshare pin_config.h)
include/lv_conf.h     LVGL config (fonts, chart, 16 ms refr, 10 ms indev)
platformio.ini        pioarduino platform, qio_qspi, huge_app.csv, -Wall -Wextra
```

**Data flow (single source of truth = the `main.cpp` router):**
```
BLE notify / scan / disconnect  (NimBLE host task)
      │  marshalled via FreeRTOS queue (evtQueue_)
      ▼
El15Client::drainEvents()  (loop task)  ──► onStatus / onDeviceFound / onState
      ▼
main.cpp handleStatus() ──► ui::onStatus()  +  g_test.onStatus() / g_batt.onStatus()
                        └─► LinkGuard arm/disarm, from the DEVICE's loadOn echo
```

**Threading model (critical):** NimBLE callbacks run on the **NimBLE host task**
and only **enqueue**; `El15Client::loopTick()` drains on the **loop task**, so
LVGL and both engines are only ever touched single-threaded. GATT service
discovery is done synchronously in `connectTo()` on the loop task (never from
the host-task `onConnect`). Audio runs on its own task at priority 8.
**QA/dev rule:** never call LVGL or an engine from a NimBLE callback.

**UI structure (`ui.cpp`):** persistent chrome (status strip + info bar + fault
banner on `lv_layer_top()`) around a `contentStack` of **seven** screens —
`SCR_MON / SCR_ADJ / SCR_GRAPH / SCR_RTEST / SCR_CONNECT / SCR_SET / SCR_BATT` —
plus a pinned Load/RUN-TEST bar and full-screen overlays
(`OV_MENU / OV_KEYPAD / OV_PICKER / OV_TEXT / OV_WIFI`). The Menu is an **8-tile
4×2 grid**. `engineBusy()` gates every manual control while a test runs.

---

## 5. Feature inventory & test procedures

Test each against the **phone simulator** first (deterministic, no current),
then a **real EL15**. "Expected" = intended behavior; note deviations.

### 5.1 Navigation
- **Menu** (top-right) → 8 tiles: Monitor / Adjust / Mode / Graph / R-Test /
  Connect / Settings / Battery. **Back arrow** on every non-Monitor screen.
- **Running-test chip** in the status strip whenever a test is live or holding
  an unsaved result ("BATT 01:23", "BATT PAUSED", "R-TEST 4/8", "… result").
  Hidden on that test's own screen; tapping it returns to the test.
- *Test:* start a test, navigate away via the Menu (and via Connect ▸ Scan),
  then use the chip to come back. The test must still be running and its result
  must still be reachable — a scan in particular must NOT end the test.
- **Touch-snap:** each press snaps to the nearest real tap target within 40 px
  (z-order/overlay aware, scrolling preserved) — small targets are forgiving.
- *Test:* reach every screen via Menu; back-arrow home from each; open/close each
  overlay. Expected: no dead tiles, no stuck screens, status strip persists,
  the Menu grid scrolls if it ever overflows.

### 5.2 Connect (`SCR_CONNECT`)
- Status row (dot + label). **Scan for devices** / **Disconnect**. Device list
  shows discovered **named** devices (name + MAC), deduped by address.
- *Test:* Scan → named devices only, no duplicates. Tap a row → "Connecting…" →
  "Connected" → auto-returns to Monitor. Serial shows
  `[ble] connecting … (addr type N)` plus the GATT characteristics dump.
- *Edge:* unnamed advertisers must NOT appear; a peer without FFF0 →
  "Not an EL15 (no FFF0)"; missing FFF1/FFF3 → "EL15 characteristics missing";
  link failure → "Connect failed" (`connect() FAILED rc=` on serial).
- **Auto-connect:** Settings ▸ Connection toggles reconnecting to the last
  device ~1.5 s after boot. Off by default. Crash recovery takes precedence.
- **Known gap:** when an 8 s scan window simply *expires*, the state is never
  reset — the chip stays "Scanning". Tap Scan again or connect. (QA_REPORT M3.)

### 5.3 Monitor (`SCR_MON`, home)
- **Status strip:** connection group (Monitor only, taps → Connect) / back arrow;
  temp chip (white < 42 °C, amber 42–50, red > 50); Menu button.
- **Info bar:** power W · fan % · temp · runtime hh:mm:ss · (CAP) Ah · (DCR) mΩ.
  Fan is clamped to the 0–5 rating before being shown as a percentage.
- **Mode | Set bar:** left = mode abbr + name (tap → picker); right = **Set**
  value + unit (tap → Adjust), or **Fuse** (tap → cycle) in RT mode.
- **Two hero blocks:** Voltage (green), Current (amber → **red + "SINKING"**
  when the device reports the load on).
- *Test:* with the load on in CC, confirm V sags, I ≈ setpoint, power/fan/
  runtime/temp update at the configured rate, current hero turns red. Switch
  modes and confirm the Set unit/label follow (A/V/**ohm**/W — CR must render
  "ohm", never a tofu box); CAP shows Ah, DCR shows mΩ.

### 5.4 Load / RUN TEST bar (pinned, Monitor/Adjust/Graph)
- Normal modes: **LOAD OFF** ↔ **LOAD ON**, reflecting the **hardware-reported**
  load state, not the tap. RT mode: **RUN TEST**. BATT mode routes to `SCR_BATT`.
- *Test (safety):* toggle load; state must track the device echo. With a warning
  active, load-**ON** must be refused while load-**OFF** always works. While an
  engine runs, the bar is inert (`engineBusy()`).

### 5.5 Adjust (`SCR_ADJ`) and keypad (`OV_KEYPAD`)
- Value card (mode name + range caption + big value/unit); unit-aware ±step chips
  (A 0.01/0.1/1 · V 0.1/1/10 · ohm 0.1/1/10 · W 1/10/50); big −/+ pads with
  hold-to-repeat; the value card itself opens the keypad.
- Ranges: A 0–40 (2 dp) · V 0–150 (1 dp) · ohm 0.05–9999 (1 dp) · W 0–400 (0 dp).
- *Test:* pick a step, tap ± → value changes by exactly that step, clamped and
  rounded; each change sends a setpoint; device echo doesn't fight your edits
  while on this screen. Keypad: type a value, SET applies it; `.` inserts one
  decimal; ⌫ works; ✕ discards. **While an engine runs, setpoint edits are
  refused** (including battery cutoff/current, which the engine copied at start).
- **Known gap:** the keypad path for the **fuse** value is unreachable —
  `openKeypad(2)` is never called, so the fuse can only be *cycled*.
  (QA_REPORT L1.)

### 5.6 Mode picker (`OV_PICKER`)
- 8 tiles: CC/CV/CR/CP/CAP/DCR + **RT** + **BATT** (the last two are UI-only
  pseudo-modes and send no device mode).
- *Test:* each real mode round-trips (device echoes it back into the badge);
  RT/BATT flip the Monitor affordances; the active mode is highlighted;
  **the picker refuses to commit while an engine is running**.

### 5.7 Resistance Test (`SCR_RTEST`) — two entry paths
The sweep is a **continuous triangular current ramp**: start → max → start over
a set duration, fitting every status packet. There is no step count, no settle
window and no collect window.
- **Menu → R-Test:** setup — Fuse tile (cycles 1/2/3/5/7.5/10/15/20/25/30/40 A),
  **Start current**, **Max current** (0 = auto, i.e. whatever the fuse safely
  allows), **Duration** (5–900 s, default 30), **2-wire / 4-wire** toggle,
  **Measure (short the probes)** tare button (2-wire only), optional circuit
  estimator inputs, **Start sweep** (disabled until a fuse is set). The hint
  line states the peak current, the duration and roughly how many readings that
  will collect; asking for more than the fuse allows turns the max amber and
  says what it will be capped to.
- **RT mode → RUN TEST on Monitor:** uses the same sweep settings.
- **Running:** RAMPING UP / RAMPING DOWN, elapsed / total, progress bar, big
  live V and I, the **live resistance estimate**, the commanded target, a
  **V + I vs time** chart (dual Y axis) and a **resistance vs time** chart that
  appears once the fit has enough current span to mean anything. **STOP**.
- *Test:* watch that the current rises smoothly rather than in visible steps,
  that it turns around at the halfway mark, and that the live R settles to a
  steady value well before the sweep ends. Verify the reported "Current sweep"
  range is what was actually drawn (it is measured, not commanded, so it will
  fall slightly short of the target at the peak).
- **Result:** big series resistance, V–I line chart (measured amber, fit green),
  and 18 detail rows: Open-circuit voltage · Probe wiring · Measured (incl.
  leads) · **Uncertainty (±)** · Fit quality (R²) · Est. short-circuit I · Sag at
  max current · Peak test power · Load temp · Max fan · Current sweep ·
  Steps/samples · Fuse limit · (estimator) Wire · Contacts · Fuse (est) ·
  Est. build R · Residual vs est. Plus **Save to SD card** and **New test**.
- *Test:* against a known resistance, R should land within the reported ±.
  Verify **STOP returns to Idle and leaves the load OFF**, that the sweep is
  bidirectional (the step count is 2n−1 for n levels), and that a tare captured
  with shorted probes subtracts from the next 2-wire run (the raw figure is
  still shown as "Measured (incl. leads)").

### 5.8 Battery capacity (`SCR_BATT`)
- **Setup:** a **chemistry grid** (one `lv_btnmatrix`, 3 per row), cell count −/+
  with a per-chemistry max, auto-filled cutoff (= cells × per-cell cutoff,
  editable), discharge current, **optional rated capacity (mAh)**, a **C-rate
  chip row**, **Start**. With a rating entered the hint line states the C-rate
  and the runtime a healthy pack should manage.
- **Chemistries** (per cell: nominal / full / cutoff · max series count):

  | Chip | Chemistry | V/cell | Max | Full pack |
  |---|---|---|---|---|
  | Li-ion | Li-ion (NMC/LCO) | 3.7 / 4.2 / 3.0 | 14S | 58.8 V |
  | LiHV | LiPo HV (4.35 V) | 3.8 / 4.35 / 3.0 | 13S | 56.6 V |
  | LiFePO4 | LiFePO4 | 3.2 / 3.65 / 2.5 | 16S | 58.4 V |
  | LTO | Lithium titanate | 2.4 / 2.8 / 1.8 | 21S | 58.8 V |
  | Na-ion | Sodium-ion | 3.1 / 4.0 / 1.5 | 14S | 56.0 V |
  | Pb 12V | Lead-acid 12 V | 2.0 / 2.13 / 1.75 | **fixed 6S** | 12.8 V |
  | NiMH | NiMH | 1.2 / 1.4 / 1.0 | 40S | 56.0 V |
  | NiCd | NiCd | 1.2 / 1.35 / 0.9 | 40S | 54.0 V |
  | Alkaline | Alkaline (primary) | 1.5 / 1.6 / 0.8 | 36S | 57.6 V |
  | Custom | — | no cell model | — | keypad cutoff only |

  Every max is set so a **fully charged** pack of that size stays under the
  EL15's 60 V input rating. **Lead-acid is 12 V only** (six 2 V cells): its
  cell-count row is hidden and the cutoff auto-fills to the standard 10.5 V.
  Alkaline is a **primary** cell — the test consumes it.
  *Test:* tap each chip and confirm the detail line below it restates the choice
  in pack volts ("6S pack = 12.0 V nominal, 10.5 V empty to 12.8 V full"); that
  the cells −/+ row disappears for **Pb 12V** and **Custom**; that the cutoff
  re-derives on every change unless you typed one; and that a chip tap is
  refused outright while a test is running.
- **C-rate → current:** the four chips carry the chemistry's own conventional
  rates (lead-acid 0.05/0.1/0.2/0.5C — it is rated at the C20 hour rate; every
  other chemistry 0.1/0.2/0.5/1C). Tapping one sets the discharge current to
  `C × rated capacity`, clamped to the load's 12 A / 150 W envelope (the hint
  says so when it clamps). Entering a rating re-applies the selected chip, so
  telling the controller the pack size is enough to get a current. Typing a
  current by hand deselects the chips until one is tapped again.
  *Test:* rating 3000 mAh + 0.2C → 0.60 A; switch to Lead-acid and the same chip
  index becomes 0.1C → 0.30 A; type 1.5 A and the chips go quiet.
- **Running:** elapsed, hero V, I, Ah, Wh, temp, live discharge curve (fixed
  time frame, smoothing, stepped auto-zoom Y scale), **STOP**, plus a
  charge/time line: **"~62% charge left - approx 1:04:20 to cutoff"**, and
  "% of rated drawn" when a rating was entered.
- **Time remaining** comes from the chemistry's discharge curve
  (`battery_model.h`), not from the rating: the pack's internal resistance is
  measured from the switch-on sag, every later reading is referred back through
  it to an open-circuit voltage, and state of charge is read off the curve. The
  pack's real capacity is then learned from Ah drawn per unit of charge state
  travelled, so **an estimate appears with no rating entered at all**, and it
  targets the cutoff rather than the rating. Custom chemistry has no curve and
  falls back to the old rated-capacity estimate, which the UI labels
  "(from the rating)".
  *Test:* start a discharge on a part-charged pack with NO rating entered — the
  ETA should appear a few seconds in (once R is measured) and settle within the
  first ~10 % of charge travel. Serial prints `[batt] pack+lead resistance … ;
  cutoff is ~N% SoC` and `[batt] start of charge ~N%`. Expect the SoC figure to
  be *approximate*, especially on the LiFePO4 plateau.
- **Paused:** an amber card gives the reason (controller battery critical, BLE
  link lost) and a **RESUME** button. The load is off and the clock is stopped,
  but nothing is discarded.
- **Result:** Ah/mAh, Wh, duration, start/end/rebound V, avg V/I, temp range,
  stop reason, "Paused for" (only if it happened), and — with a rating —
  rated capacity, **state of health** (green ≥80 %, amber ≥60 %, red below) and
  C-rate. When the battery model established itself the result also carries
  **pack + lead resistance**, the **charge span the run covered**
  ("~94% → ~4%", amber when it did not start near full — a partial discharge is
  exactly when the measured Ah understates the pack), and an **implied full
  capacity** extrapolated from that span. **Saved to SD automatically**; the
  button is the retry.
- *Test the pause path:* start a discharge, then pull the EL15's power (or walk
  out of range). Expect PAUSED + a reason, not a finished test; restore the link
  and tap RESUME; confirm Ah continues from where it was rather than restarting.
- *Test the datapoint log:* after a run of a few minutes, open `BATT_NNN.CSV`
  and confirm the `# Datapoints` block has one row per ~2 s with sane
  voltage/current/power/mAh/temperature columns.
- *Test the R-test datapoint log (new 2026-08-05):* after a sweep, open
  `RTEST_NNN.CSV` and confirm the `# Datapoints` block holds roughly one row
  per status packet (~20/s at the default poll rate — a 30 s sweep is ~600
  rows), that `current_a` tracks `target_a` with a small visible lag, and that
  a load-off dropout (if one occurred) reads as `current_a` ≈ 0 under a nonzero
  target rather than a missing row.
- *Test:* priming holds the load off ~1.5 s and reads Voc; sanity aborts fire
  (source < 0.1 V, > 60 V, or already at/below cutoff); discharge starts at the
  clamped current `min(request, 12 A, 150 W ÷ Voc)`; Ah/Wh integrate; the
  debounced cutoff ends it (3 consecutive samples ≤ cutoff, or one sample
  < cutoff − 0.3 V); rest/rebound is captured; STOP mid-discharge still produces
  a valid partial result.

### 5.9 Graph (`SCR_GRAPH`)
- Live V (green) / I (amber) numbers + a two-series auto-scaling `lv_chart`.
- *Test:* both traces auto-scale and scroll; range/window labels update; no
  flat-line degeneracy.

### 5.10 Settings (`SCR_SET`)
Eight cards: **SAMPLE RATE** (20/10/4/2 Hz chips — 20 Hz = 50 ms is the
default) · **PROBE WIRING** (below) · **CONNECTION** (auto-connect toggle) · **BATTERY** (controller's own
%, mV, charge state) · **CLOCK** (RTC readout, Wi-Fi network picker, password
entry, UTC offset, "Sync clock now") · **SCREEN PROTECTION** (pixel shift +
**screen timeout**: Never / 30 s / 1 / 5 / 10 / 30 min, with a line stating that
it dims at that interval and blanks at 5×) · **SD CARD** (Check card — always
re-initialises, so it is also how you confirm a freshly-inserted card) ·
**SYSTEM** (brightness, volume, mute, firmware/heap info, restart).
- **Every save is verified.** `sd::saveCsv()` checksums the report on the way out
  (chunked CRC-32, 8 KB granularity), reads it back off the card and compares.
  A save therefore takes about **twice** as long — ~20 s for a full capacity
  report — and the button says "Writing + verifying..." while it does. Serial
  prints `[sd] verify OK: <file>, N B read back and matched`.
  *Test:* save a report and confirm that line appears, and that the button goes
  green with the file name only after it does.
  A card that loses data gets **one retry onto fresh clusters** (the rejected
  file is left in place meanwhile, precisely so the retry lands elsewhere); both
  copies are deleted before returning, and two failures report
  "Card lost data twice - replace the card". Serial localises the damage:
  `[sd] verify MISMATCH: bytes 114688..122879 of BATT_007.CSV`.
- *Test:* change brightness / volume / sample rate → reboot → they stick.
  A **successful clock sync deliberately reboots** the board (see §8).
  Wi-Fi scan and sync are **refused while a test runs**.

#### PROBE WIRING — device-wide, all modes
The EL15 senses voltage at **its own terminals**, so a 2-wire hook-up reads short
by the drop across the leads and clips: `V_dut = V_terminals + I × R_lead`. This
card holds the wiring for the whole controller, not just the R-test.

- **2-wire + lead resistance** → every status packet is corrected by
  `+ I × R_lead` in `main.cpp compensateProbe()`, *before* it fans out, so the
  screen and the engines can never disagree. Enter the figure by tapping **Lead
  resistance** (milliohms, capped at 1 Ω), or let **R-Test ▸ Measure (short the
  probes)** capture it — both write the same value.
- **4-wire (Kelvin)** → no correction at all; the sense path carries no current,
  so the reading already belongs to the part. The lead-resistance row is hidden.
- The Monitor's voltage caption reads **VOLTAGE**, **VOLTAGE - 4-WIRE**, or
  **VOLTAGE - LEAD-CORRECTED**, so a reading that deliberately differs from the
  EL15's own front panel always says why.
- **The R-test is excluded** from the correction on purpose: it already subtracts
  the tare from its own fitted slope, and pre-correcting the volts would remove
  the same resistance twice (a low-milliohm result could land at zero).
- *Test:* set a lead resistance of, say, 50 mΩ with a load drawing 2 A and
  confirm the Monitor voltage sits ~0.10 V above the EL15's own panel and the
  caption says LEAD-CORRECTED; switch to 4-wire and it returns to the panel
  figure with the row hidden; toggle from **either** screen (Settings or R-Test
  setup) and confirm the other screen agrees — they are one setting, not two.
- *Test the capacity effect:* a 2-wire discharge with a lead figure set should
  now cut off later than the same run uncorrected — the cutoff is being applied
  at the pack rather than at the load's terminals. `BATT_NNN.CSV` records the
  wiring, the corrected lead resistance, and whether voltages were corrected.

### 5.11 Audio & buttons
- Tones: click on taps, firmer confirm on physical buttons, rising chime on test
  complete, falling on error, urgent alarm on fault/e-stop. Volume + mute in
  Settings; init failure is non-fatal (calls become no-ops).
- **BOOT = hardware emergency stop** from any screen: stops both engines, pushes
  LOAD OFF + setpoint 0, alarm, red ack banner (and honestly says so if it could
  not reach the load).
- **PWR short = display sleep/wake** (true black, touch inert).
  **PWR long = load-safe power-off**: stops the load, flushes the write, then
  cuts the rails via the AXP2101.

---

## 6. Safety-critical behavior (scrutinize hard)

This firmware drives **real current**. Verify every one of these:

- **Load ON reflects hardware state**, not the tap. A commanded-but-rejected
  load must not show ON.
- **Fault gating:** while a status packet carries a protection warning
  (REV/UVP/other) the red banner shows and **load-ON is refused**; **load-OFF is
  never blocked**. Note the banner **mirrors the device** rather than latching —
  it clears when the device stops reporting the warning (see QA_REPORT M1: this
  is the decided semantics, not an oversight).
- **Two engines, one load:** R-test and capacity are mutually exclusive, and
  manual load/setpoint/mode controls are gated while either runs.
- **Safe teardown on disconnect:** `stopAll()` captures "hot" *before* stopping
  anything, then uses `shutdownAndDisconnect()` (LOAD_OFF + 40 ms flush) whenever
  an engine was running **or the device reported the load on**. A plain
  disconnect can no longer walk away from flowing current.
- **Link-loss supervisor:** whenever the load reports ON the guard is armed; a
  drop from a live link starts up to 8 reconnect-and-force-LOAD-OFF attempts
  with a red banner and repeating alarm, and gives up **loudly** with a tappable
  retry. Manual scan/connect makes it stand down.
- **Crash/reboot recovery:** an NVS in-flight flag is written *synchronously*
  while energised; the next boot offers "reconnect and force LOAD OFF".
- **Controller-brownout auto-safe:** on battery (not USB), at ≤ 8 % or ≤ 3.30 V
  for 3 consecutive 1 Hz reads, the load is force-stopped before the controller
  can brown out and strand it.
- **Sweep clamps:** never commands more than `min(80 % fuse, 12 A, 150 W ÷ Voc,
  40 A)`; aborts if the source is outside 0.1–60 V or a protection trips; always
  LOAD_OFF on finish/abort.
- **RT/BATT never command a device "RT"/"BATT" mode** — both engines put the
  load in **CC**. Confirm the device mode during a test is CC.
- **Corrupt packets are dropped**, not parsed: a status frame failing the
  checksum never reaches the engines (logged, rate-limited).

---

## 7. Wire protocol (for verifying BLE behavior)

GATT service `FFF0`; **notify `FFF1`** (`--wN-`), **command `FFF3`**
(`--w--`, **write-no-response only**). The per-connect serial dump prints this.

**Frame format:** `AF 07 03 <cmd> <len> <data…> <checksum>`, where the checksum
byte makes the **whole frame sum to 0 (mod 256)** — the same rule the status
parser enforces on incoming packets. **A real EL15 silently drops any command
that doesn't sum to zero.**

| Action | Frame (hex) |
|---|---|
| Poll status | `AF 07 03 08 00 3F` |
| Load ON | `AF 07 03 09 01 04 39` |
| Load OFF | `AF 07 03 09 01 00 3D` |
| Lock keypad | `AF 07 03 09 01 01 3C` |
| Set mode | `AF 07 03 03 01 <mode> <cksum>` |
| Set setpoint | `AF 07 03 04 04 <float32 LE> <cksum>` |

Mode IDs: CC `0x01`, CAP `0x02`, CV `0x09`, DCR `0x0A`, CR `0x11`, CP `0x19` —
**all six verified against a real unit** (commanded mode == reported `b5`).

**Status notification** (28 bytes, header `DF 07 03 08`, checksum
`sum(bytes) & 0xFF == 0`): voltage f32 @7, current f32 @11, runtime i32 @15,
power = V×I; byte 5 = mode (low 5 bits) + fan (bits 6–7), byte 6 =
load/lock/fan-MSB/protection nibble; mode-specific tail (temp/setpoint, or CAP
energy/capacity, or DCR I1/I2/mΩ).

**Behaviors that only showed up on real hardware — preserve them:**
- **Telemetry is POLL-driven.** The device sends a status frame only in reply to
  a POLL write; it does not free-run. So a working POLL is proof FFF3 writes land.
- **Control writes must be paced.** FFF3 is write-no-response only and the device
  drops a command landing too close behind another one. `writeRaw` holds every
  *control* write ≥50 ms from the previous one. Polls themselves are never paced,
  but they are held `ctrlPollGapMs` (25 ms) clear of a control write.
- **LOAD_ON at a 0.000 A CC setpoint does nothing.** The write is accepted, but
  the device reports `load=0`, sinks no current, and never recovers on its own —
  a sweep starting from 0 A ran to completion with `I=0` throughout. Always
  command a non-zero current *before* LOAD_ON. The sweep engine floors its ramp
  at 0.05 A and re-asserts LOAD_ON if a mid-sweep packet reports the load off.
- **The initial LOAD_ON does not always land**, so the re-assert also has to be
  prompt — at a 1 s interval it cost the first second of a ramp, which is the
  low-current end where the fit most needs span.
- **A protection can stay latched from a previous run.** Priming tolerates a
  warning for a 4 s grace period while pushing CC / setpoint 0 / LOAD OFF (the
  state that lets the device shed a trip) before giving up. Mid-sweep, a trip
  aborts immediately.
- **Poll rate:** the EL15 produces ~17–19 fresh samples/s (min ~23 ms between
  distinct frames), so **50 ms (20 Hz) is the practical max** and is the default.
  Faster just refetches repeats and floods the link.
- **MTU:** the firmware requests 247 so a 28-byte frame fits one notification.
  Reassembly + header resync is still implemented and should be tested against a
  peer that keeps the 23-byte default.

*QA angle:* sniff FFF3 writes and check the trailing checksum byte; verify the
poll cadence matches the Settings chip; verify setpoint floats are little-endian.

---

## 8. Known gaps, stubs & risk areas

**Untested paths (highest value for QA):**
- **SD Save from the UI** — Settings ▸ SD ▸ Check card, then an R-test/battery
  Save. Expect a green button with the file name, or an honest red reason
  ("No card detected (reseat it)" / "Card not formatted (use FAT32)" /
  "Cannot write (card locked or full)"). A half-written file is deleted rather
  than left claiming to be a result. Confirm the CSV opens in a spreadsheet.
- **Real current** through either engine.
- **Link-guard / crash-recovery / brownout** paths, none of which has fired for
  real. Drill them per `FIRST_CONTACT.md` §15/16/16b.
- **NVS persistence, burn-in shift/dim, NTP sync, Kelvin + tare** — all compile
  and boot, none confirmed end-to-end.

**Known open defects** (see QA_REPORT.md for the full list and status):
- **M3** — a scan window that simply expires never resets the state; the UI keeps
  showing "Scanning".
- **L1** — the fuse keypad path is unreachable (cycle-only).
- **L4** — DCR mode shows `dcrI1` on the current hero; decide whether that or a
  zeroed current is intended.

**Deliberate limitations:**
- **Capacity CSV holds the summary only** — the per-sample discharge curve is
  Phase 3 of `CAPACITY_PLAN.md`.
- **CSV timestamps** fall back to `(RTC not set) uptime NNN s` until the clock is
  set via Settings ▸ Clock.
- **A successful clock sync auto-reboots.** Wi-Fi needs ~50 KB contiguous, so the
  draw buffer is temporarily shrunk to 16 lines and **cannot be reassembled**
  afterwards (heap fragmentation) — the reboot is how the full buffer comes back.
  RTC + NVS persist, so nothing is lost.
- Android-app features **not ported**: runtime/step/OCP bench tests, on-device
  history, PDF export, alarms, calibration sweep.

**Approximations (design fidelity):**
- **Fonts:** Montserrat stands in for Inter/JetBrains-Mono; hero digits are
  **48 px** (LVGL's largest built-in) vs the 64 px spec.
- **Icons:** nearest LVGL built-in symbols, not exact Phosphor glyphs.
- **Glyphs:** Ω→`ohm`, °→`C`, en/em-dash→`-`, ellipsis→`...`, `±`→`+/-`
  (Montserrat lacks those glyphs; using them renders empty boxes).

**Risk areas worth targeted testing:**
- **RAM is the binding constraint.** 320 KB, no PSRAM, LVGL allocating from the
  system heap. NimBLE needs ~25–30 KB **contiguous** to *establish* a connection;
  a too-large draw buffer presents as "connect always fails, HCI 0x3e" while
  scanning still works. If you grow the UI or `BUF_LINES`, re-verify connects.
- **Setpoint sync races:** echoes are suppressed while on Adjust/keypad and in
  RT/BATT mode — verify no flicker when adjusting with the load live.
- **Adjust ranges exceed hardware ratings** (A to 40, V to 150) — the device
  enforces its own 12 A / 150 W / 60 V limits; verify sane behavior at the edges.
- **Touch:** both FocalTech and CST anti-sleep registers are written at init.
  Verify sustained responsiveness over long sessions and during heavy redraws.
- **Reassembly resync** on lost/garbled bytes (drop-one-byte header resync).
- **A wedged SD card** (from an aborted write) reports CMD0 `R1=0x00` and only a
  **physical reseat / power-cycle** clears it — an ESP reset will not.

---

## 9. Suggested QA checklist

Boot & display
- [ ] Cold boot: UI up in < ~3 s, correct colors, no tofu boxes, no panic on serial.
- [ ] `[boot] reset reason: power-on` and `[audio] ready (ES8311)` both present.
- [ ] Every screen renders within 368 × 448 (no clipping); Menu is 8 tiles.

Touch
- [ ] Every button/tile responds with visible press-dim; no missed taps over 5 min.
- [ ] Touch-snap makes small targets forgiving without breaking scrolling.

Connect
- [ ] Scan lists named devices only, no duplicates; GATT dump appears on connect.
- [ ] Connect → Connected → auto-home; Disconnect works; states match serial.
- [ ] Random-address peer (phone sim) connects; `addr type 1` in the log.
- [ ] Auto-connect toggle reconnects on the next boot (and only then).

Control
- [ ] All 6 device modes select and echo back; CR renders "ohm", not a box.
- [ ] Adjust ± / step chips / keypad set the setpoint exactly, clamped & rounded.
- [ ] Load ON/OFF tracks hardware state; ON refused while a warning is active,
      OFF never refused.
- [ ] Every manual control is inert while an engine runs.

Resistance test
- [ ] Both entry paths run; result lands within the reported ±.
- [ ] Progress / STOP work; STOP returns to Idle **and** leaves the load OFF.
- [ ] 4-wire toggle and 2-wire tare both reflected in the result rows and CSV.
- [ ] **Save to SD** → green with `RTEST_NNN.CSV`, or an honest red reason.

Battery capacity
- [ ] Setup validation: cutoff above/below Voc, out-of-range source, zero current.
- [ ] Discharge integrates Ah/Wh; debounced cutoff fires; rebound recorded.
- [ ] STOP mid-discharge yields a valid partial result, load OFF.
- [ ] **Save to SD** → green with `BATT_NNN.CSV`.

Graph & settings
- [ ] Both traces auto-scale/scroll; range + window labels update.
- [ ] Brightness / volume / sample rate survive a reboot.
- [ ] Clock sync sets the RTC (and reboots); CSV timestamps become real.

Safety (do these last, per FIRST_CONTACT.md)
- [ ] Fault banner shows, gates load-ON, never gates load-OFF.
- [ ] Disconnect while hot stops the load first.
- [ ] BOOT e-stop kills the load from any screen.
- [ ] Link-guard hot drop → banner, alarm, reconnect, "LOAD FORCED OFF".
- [ ] Crash recovery: pull power while hot → boot offers reconnect-and-kill.
- [ ] PWR long-press powers off **after** the load is confirmed off.
- [ ] Sweep never exceeds clamp limits; aborts an out-of-range source.

---

*Keep this current as features land. File issues against the specific
`firmware/src/*` file + screen/state.*
