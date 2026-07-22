# EL15 Controller Firmware — QA Guide

Advanced-QA reference for the **standalone ESP32-C6 firmware** in `firmware/`
(the on-device controller for the ALIENTEK EL15 electronic load). Covers current
status, how to build/flash/drive it, the code layout, a per-feature test matrix,
the wire protocol, safety-critical behavior, and known gaps/risks.

> Scope: this document is about the **`firmware/` ESP32-C6 target** running the
> **v2 "Focus"** touchscreen UI. The repo also contains the Android app
> (`app/`) and a phone-based BLE load simulator (added in commit `e22c845`);
> those are referenced only as test tools here.

Reference commits (branch `claude/android-apk-load-tester-k82q4g`):
- `d65d430` connect using the scanned BLE address type
- `c52e337` v2 "Focus" touchscreen UI + FT3168 touch bring-up
- `e22c845` EL15 Load Simulator phone app (BLE peripheral) — test tool
- `47277d9` first build/flash bring-up on real hardware

---

## 1. Current status (what's verified vs. not)

| Area | State |
|---|---|
| Build (PlatformIO, pioarduino platform, ESP32-C6) | ✅ compiles clean; image ~1.4 MB in a 3 MB `huge_app` partition |
| Boot on hardware | ✅ boots clean, no panic/bootloop |
| Display (SH8601 AMOLED, QSPI) | ✅ renders v2 UI, correct colors |
| Touch (FT3168) | ✅ works after power-mode init; 10 ms sampling |
| Demo simulator (on-device) | ✅ connects; drives full UI without hardware |
| BLE connect to **phone** simulator (random address) | ✅ reaches **Connected** |
| BLE **live data** end-to-end (phone/real → Monitor heroes update) | ⚠️ **NOT yet confirmed** — connect works; verify readouts stream/update |
| Real ALIENTEK EL15 hardware | ⚠️ **NOT tested** in-house (no unit on hand) |
| SD-card save (R-Test result) | ⚠️ **STUBBED** — UI confirms "Saved — RTEST_NNN.csv"; no file is written |
| Resistance-test engine math (R, Voc, R²) | ✅ byte-level port of the Kotlin, audited (see §7) |

**Top things QA should confirm first:** (1) live V/I/mode data actually streams
into the Monitor hero blocks from the phone sim and, if available, a real EL15;
(2) load ON/OFF and the fault path behave safely; (3) touch stays responsive
across all screens during live updates.

---

## 2. Hardware & the three test surfaces

**Board:** Waveshare **ESP32-C6-Touch-AMOLED-1.8** — 1.8″ AMOLED **368 × 448 px
portrait**, SH8601 controller (QSPI), FT3168 capacitive touch (I²C), TCA9554 I/O
expander (panel enable), AXP2101 PMIC. RISC-V, BLE 5, 16 MB flash.
Enumerates as **COM4** (native USB-Serial/JTAG) on the test PC.

You can drive the firmware three ways — exercise all three:

1. **On-device Demo Simulator** (no BLE, no hardware). Connect → *Demo
   Simulator*. Models an ideal source (~12.6 V) behind ~0.35 Ω; honors
   mode/setpoint/load and streams synthetic readings. Fastest way to test UI
   logic and the resistance sweep end-to-end.
2. **Phone BLE simulator** (real BLE path, no load hardware). The app from commit
   `e22c845` advertises the EL15 GATT service as a BLE peripheral. Exercises the
   real scan/connect/notify/write path. **Random address** — this is what the
   `d65d430` fix enables.
3. **Real ALIENTEK EL15** (the actual target). Untested in-house — highest-value
   QA. Drives real current; observe all safety behavior.

---

## 3. Build / flash / monitor

PlatformIO Core is installed **off-PATH** at
`~/.platformio/penv/Scripts/pio.exe`. There is no `idf.py`/`esptool` on PATH.

```bash
PIO=~/.platformio/penv/Scripts/pio.exe
"$PIO" run   -d firmware                                   # build
"$PIO" run   -d firmware -t upload --upload-port COM4      # flash
"$PIO" device monitor -p COM4 -b 115200                    # serial log
```

- First build downloads ~1 GB (platform + RISC-V toolchain + arduino-esp32 3.1.3
  + libs); later builds are incremental (~20 s). Changing `include/lv_conf.h`
  forces a full LVGL recompile (~75–125 s).
- Serial is 115200. A boot line `esp_core_dump_flash: Incorrect size of core
  dump image` is **harmless** (no coredump partition). Occasional
  `Wire.cpp requestFrom Error -1` are benign transient touch-I²C hiccups.
- Connect attempts log `[ble] connecting to <addr> (addr type N)` where
  `type 0`=public, `type 1`=random.

Library versions resolve **newer** than the `^` pins in `platformio.ini`:
GFX **1.6.7**, NimBLE **2.5.0**, LVGL **8.4.0**. Code is adapted to these; watch
for drift on `pio update`.

---

## 4. Code layout & architecture

```
firmware/src/
  main.cpp            owns objects, routes events, demo pump   (≈ Android DeviceCore)
  el15_protocol.h     wire protocol: packet parse + command frames (header-only, pure)
  el15_client.{h,cpp} BLE central: scan/connect/subscribe/poll/reassemble
  el15_controller.h   controller interface + El15Simulator (demo load)
  resistance_test.h   fuse-aware current-sweep engine (least-squares R, R²)
  display.{h,cpp}     SH8601 + TCA9554 panel enable + FT3168 touch + LVGL glue
  ui.cpp / ui.h       v2 "Focus" LVGL UI (1309 lines — the bulk of the app)
  board_config.h      ALL board GPIO/pins (verified vs Waveshare pin_config.h)
include/lv_conf.h     LVGL config (fonts, chart, system-heap memory, 10 ms indev)
platformio.ini        pioarduino platform, qio_qspi, huge_app.csv partition
```

**Data flow (single source of truth = the `main.cpp` router):**
```
BLE notify / scan / disconnect  (NimBLE host task)
      │  marshalled via FreeRTOS queue (evtQueue_)
      ▼
El15Client::drainEvents()  (loop task)  ──► onStatus / onDeviceFound / onState
      │                                          │
      │  demo path: El15Simulator::tick() ───────┤   (also loop task)
      ▼                                          ▼
main.cpp handleStatus() ──► ui::onStatus()  +  g_test.onStatus()
```

**Threading model (critical):** NimBLE callbacks run on the **NimBLE host task**,
not the Arduino loop. They only **enqueue**; `El15Client::loopTick()` drains the
queue on the **loop task**, so LVGL and the resistance-test engine are only ever
touched single-threaded. GATT service discovery is done synchronously in
`connectTo()` on the loop task (never from the host-task `onConnect`). **QA/dev
rule:** never call LVGL or `g_test` from a NimBLE callback.

**UI structure (`ui.cpp`):** persistent chrome (status strip 42 px + info bar
26 px + fault banner) around a `contentStack` of five screens
(`SCR_MON / SCR_ADJ / SCR_GRAPH / SCR_RTEST / SCR_CONNECT`), plus a pinned
Load/RUN-TEST bar, plus three full-screen overlays on `lv_layer_top()`
(`OV_MENU / OV_KEYPAD / OV_PICKER`). `showScreen()` toggles screen + chrome
visibility; `showOverlay()` toggles overlays. Live data enters through
`ui::onStatus/onConnState/onDeviceFound/onTest*`.

---

## 5. Feature inventory & test procedures

Test each on **Demo** first (deterministic), then the **phone sim**, then (if
available) a **real EL15**. "Expected" = intended behavior; note deviations.

### 5.1 Navigation
- **Menu button** (top-right, list icon) → full-screen 6-tile Menu
  (Monitor / Adjust / Mode / Graph / R-Test / Connect). Any destination ≤ 2 taps.
- **Back arrow** (top-left, on every non-Monitor screen) → returns to Monitor in 1 tap.
- **Overlays** (Menu / keypad / picker) cover the bars; ✕ closes.
- *Test:* reach every screen via Menu; back-arrow home from each; open/close each
  overlay. Expected: no dead tiles, no stuck screens, status strip persists.

### 5.2 Connect (`SCR_CONNECT`)
- Status row (dot + label). **Scan for devices** (shown when disconnected);
  **Disconnect** (red, shown when connected). Device list: **Demo Simulator**
  pinned on top, then discovered `EL15-XXXX` rows (name + MAC).
- *Test:* Scan → list populates with **named** devices only; **duplicates are
  suppressed** (dedup by address). Tap a row → "Connecting…" → "Connected" →
  auto-returns to Monitor. Serial shows `[ble] connecting … (addr type N)`.
- *Edge:* unnamed advertisers must NOT appear; a peer with FFF0 missing →
  "Not an EL15 (no FFF0)"; missing FFF1/FFF3 → "characteristics missing";
  link failure → "Connect failed".

### 5.3 Monitor (`SCR_MON`, home)
- **Status strip:** connection group (Monitor only, taps → Connect) / back arrow;
  temp chip (color: white < 42 °C, amber 42–50, red > 50); Menu button.
- **Info bar** (connected + Monitor/Adjust/Graph only): power W · fan n/5 ·
  runtime hh:mm:ss · (CAP) Ah · (DCR) mΩ.
- **Mode | Set bar:** left = mode abbr + name (tap → mode picker); right = **Set**
  value + unit (tap → Adjust), or **Fuse** value (tap → cycle) in RT mode.
- **Two hero blocks** (flex-fill): Voltage (green) and Current
  (amber → **red + pulsing "SINKING"** when the load is on).
- *Test:* with Demo connected + load ON in CC, confirm V sags, I ≈ setpoint,
  power/fan/runtime/temp update ~2×/s, current hero turns red. Switch modes and
  confirm the Set unit/label follow (A/V/Ω/W); CAP shows Ah in info bar, DCR shows
  mΩ + zero current.

### 5.4 Load / RUN TEST bar (pinned, Monitor/Adjust/Graph)
- Normal modes: **LOAD OFF** (green outline) ↔ **LOAD ON** (solid red). Reflects
  the **hardware-reported** load state, not just the tap.
- RT mode: **RUN TEST** (accent) — runs the sweep.
- *Test (safety):* toggle load; state must track the device echo. With a fault
  latched, load must **not** turn on. Disconnecting mid-test must turn the load
  OFF (see §6).

### 5.5 Adjust (`SCR_ADJ`, dial-stepper)
- Value card (mode name + range caption + big value/unit); unit-aware **±step
  chips** (A: 0.01/0.1/1 · V: 0.1/1/10 · Ω: 0.1/1/10 · W: 1/10/50); big **−/+
  pads**; **Type exact value** → keypad.
- *Test:* pick a step, tap ± → value changes by exactly that step, clamped to the
  unit range (A 0–40, V 0–150, Ω 0.05–9999, W 0–400) and rounded to the unit's
  decimals; each change sends a setpoint to the device; live echo doesn't fight
  your edits while on this screen.

### 5.6 Numeric keypad (`OV_KEYPAD`)
- Title, right-aligned value + unit, 4 unit-aware presets, `7-8-9/4-5-6/1-2-3/.-0-⌫`,
  **SET**. `.` inserts one decimal; digits cap at 6 significant chars.
- *Test:* type a value, SET applies it (setpoint or fuse). Backspace/decimal
  rules hold. Cancel (✕) discards.

### 5.7 Mode picker (`OV_PICKER`)
- 7 tiles: CC/CV/CR/CP/CAP/DCR + **RT** (amber). Selecting a normal mode sends
  `setMode`, updates Set unit + default step, returns to Monitor. Selecting **RT**
  makes RT the UI mode (no device mode sent) — Set→Fuse, Load→RUN TEST.
- *Test:* each normal mode round-trips (device echoes the mode back into the
  hero badge); RT flips the Monitor affordances; the currently-active mode is
  highlighted.

### 5.8 Resistance Test (`SCR_RTEST`) — two entry paths
- **From Menu → R-Test:** Idle setup — Fuse tile (tap cycles
  1/2/3/5/7.5/10/15/20/25/30/40 A, red until set), Steps −/+ (3–20, default 8),
  **Start sweep** (disabled until a fuse is set).
- **From RT mode → RUN TEST** on Monitor: uses the current fuse + steps, jumps
  straight into Running.
- **Running:** spinner + "RUNNING", Step n/total, progress bar, live V/I, **STOP**.
- **Result:** big series resistance (Ω; 4 dp < 1 Ω else 3 dp), optional amber
  low-confidence banner, detail rows (Voc, R², current sweep, steps/samples, fuse
  limit, load temp), **Save to SD card**, **New test**.
- *Test (Demo):* run against the demo → expect **R ≈ 0.35 Ω, Voc ≈ 12.6 V,
  R² ≈ 1.0**. Try a high-voltage demo circuit (tap the status bar to edit) to see
  the sweep power-limit; try > 60 V to see the over-range abort. Verify STOP
  turns the load OFF and returns to Idle.

### 5.9 Graph (`SCR_GRAPH`)
- Live V (green) / I (amber) numbers + a two-series auto-scaling `lv_chart`
  (per-series Y range, ~60-sample rolling window) + range/window labels.
- *Test:* with the load cycling, both traces auto-scale and scroll; ranges/window
  labels update; no flat-line degeneracy (flat series expand to ±0.5).

---

## 6. Safety-critical behavior (scrutinize hard)

This firmware drives **real current** on a real EL15. Verify:

- **Load ON reflects hardware state**, not the tap — the button reads the device's
  reported `loadOn`. A commanded-but-rejected load must not show ON.
- **Fault gating:** on a protection trip (REV/UVP/other) a full-width red banner
  latches; while latched the **load cannot be turned on**. Tapping the banner
  clears it.
- **Safe teardown on disconnect:** disconnecting while a resistance test is
  running uses `shutdownAndDisconnect()` which pushes **LOAD_OFF** and flushes
  (40 ms) before dropping the link. Verify the load actually stops. (Note: a plain
  manual disconnect with the load on — no test running — issues `disconnect()`
  without a LOAD_OFF, matching the Android app; confirm this is acceptable.)
- **Resistance sweep clamps:** never commands more than `min(80 % fuse, 12 A,
  150 W ÷ Voc, 40 A)`; aborts if the source is outside 0.1–60 V or a protection
  trips; always LOAD_OFF on finish/abort.
- **RT mode does not command a device "RT" mode** — the engine puts the load in
  **CC** and steps current. Confirm the device mode during a test is CC.

---

## 7. Wire protocol (for verifying BLE behavior)

GATT service `FFF0`; **notify `FFF1`**, **write `FFF3`**. The firmware is a
byte-for-byte port of the Android/DM40GUI protocol (audited).

**Commands** (written to FFF3, no response):

| Action | Frame (hex) |
|---|---|
| Poll status | `AF 07 03 08 00 3F` |
| Load ON | `AF 07 03 09 01 04` |
| Load OFF | `AF 07 03 09 01 00` |
| Lock keypad | `AF 07 03 09 01 01` |
| Set mode | `AF 07 03 03 01 <mode>` |
| Set setpoint | `AF 07 03 04 04 <float32 LE>` |

Mode IDs: CC `0x01`, CAP `0x02`, CV `0x09`, DCR `0x0A`, CR `0x11`, CP `0x19`.

**Status notification** (28 bytes, header `DF 07 03 08`, CRC = `sum(bytes) &
0xFF == 0`): voltage f32 @7, current f32 @11, runtime i32 @15, power = V×I;
byte 5 = mode(low 5) + fan(bits 6–7), byte 6 = load/lock/fan-MSB/protection nibble;
mode-specific tail (temp/setpoint, or CAP energy/capacity, or DCR I1/I2/mΩ).

- **MTU:** firmware requests **247** so a 28-byte frame fits in one notification.
  Frame **reassembly** across notifications is still implemented and must be
  tested at the default 23-byte MTU (a peer that doesn't honor the larger MTU
  splits the frame → verify reassembly + header resync).
- *QA angle:* sniff FFF3 writes match the table above byte-for-byte; verify the
  poll cadence (~500 ms) and that setpoint floats are little-endian.

---

## 8. Known gaps, stubs & risk areas

**Stubs / not implemented:**
- **SD-card save** — now real (`sd_card.cpp`, SPI mode: SCK=11 MOSI=10 MISO=18
  CS=6) but **not yet exercised against a card**. It writes `RTEST_NNN.CSV` /
  `BATT_NNN.CSV` at the card root. Test: with a FAT32 card, Save should turn
  green with the file name; with the slot empty it must turn red with "No card
  detected" — a confirmation is only ever shown for a file that really landed.
  Also confirm the panel keeps drawing normally afterwards (the card and the
  display share one SPI host). Settings ▸ SD CARD ▸ *Check card* probes the slot
  without writing.
- **Capacity CSV holds the summary only** — the per-sample discharge curve is
  Phase 3 of CAPACITY_PLAN.md.
- **CSV timestamps** fall back to uptime seconds until the RTC is set (Settings
  ▸ Clock ▸ Wi-Fi NTP sync sets it); the R-Test result *screen* has no
  Timestamp row.

**New this session (compile-clean, hardware-unverified):**
- **NVS persistence** (`prefs`) — brightness, volume/mute, sample rate, screen
  protection, R-test + battery setup, Wi-Fi, last device. Verify by changing a
  setting, rebooting, and confirming it stuck.
- **Screen protection** (`display`) — pixel shift on by default; idle dim then
  blank, waking on any touch/button; blank suppressed while a test runs.
- **Link-loss supervisor + crash recovery** (`link_guard`) — locked red alarm
  banner + reconnect-and-force-LOAD-OFF on a hot drop; amber recovery offer on
  the next boot after a crash left the load on.
- **4-wire / lead tare** (`resistance_test`) — 2-wire/4-wire toggle; a shorted-
  probe tare sweep that subtracts from later 2-wire results (raw shown too).
- **NTP clock sync** (`netclock`) — Settings ▸ Clock ▸ tap "Wi-Fi network"
  scans and lists nearby SSIDs (signal bars, "Hidden/manual" for unlisted ones);
  pick one, type the password, "Sync clock now". Radio up only for the
  scan/sync, both refused while a test runs.
- Android-app features **not ported**: capacity/runtime/step/OCP bench tests,
  on-device history, settings, alarms, calibration sweep.

**Approximations (design fidelity):**
- **Fonts:** Montserrat (bundled) stands in for Inter/JetBrains-Mono; hero digits
  are **48 px** (LVGL's largest built-in) vs the 64 px spec.
- **Icons:** nearest LVGL built-in symbols, not exact Phosphor glyphs.
- **Glyphs:** Ω→`ohm`, °→`C`, en/em-dash→`-`, ellipsis→`...`, `±`→`+/-`
  (Montserrat lacks those glyphs; using them renders empty boxes).
- **RT tile "dashed amber border"** → solid amber (LVGL borders are solid-only).

**Risk areas worth targeted testing:**
- **Live-data flow not yet confirmed** end-to-end from a BLE peer (connect works;
  verify Monitor heroes + info bar actually update from FFF1 notifications).
- **Setpoint sync races:** the UI suppresses device echoes while on Adjust/keypad
  and in RT mode — verify no flicker/fighting when adjusting with the load live.
- **Adjust UI ranges exceed hardware ratings** (e.g. A up to 40, V up to 150) —
  the device enforces its own 12 A/150 W/60 V limits; verify sane behavior when a
  setpoint exceeds them.
- **Touch:** FT3168 needs its power-mode init (reg `0xA5`=Active) or it stops
  reporting; a transient I²C read now **holds last state**. Verify sustained
  responsiveness across long sessions and during heavy redraws (screen switches).
- **Scan dedup / address type:** verify random-address peers connect and the list
  doesn't grow with duplicate adverts.
- **Reassembly resync** on lost/garbled bytes (drop-one-byte header resync).
- **Real EL15 address type** unknown — if a real unit advertises random, the
  `d65d430` fix now handles it; if public, also handled. Verify on real hardware.

---

## 9. Suggested QA checklist

Boot & display
- [ ] Cold boot: UI up in < ~3 s, correct colors, no boxes/tofu, no panic on serial.
- [ ] Every screen renders within the 368 × 448 bounds (no clipping/overflow).

Touch
- [ ] Every button/tile responds with visible press-dim; no missed taps over a 5-min session.
- [ ] Taps land on the intended targets on all screens/overlays.

Connect (Demo, phone sim, real EL15)
- [ ] Scan lists named devices, no duplicates; Demo pinned on top.
- [ ] Connect → Connected → auto-home; Disconnect works; states match serial log.
- [ ] Random-address peer (phone) connects; `addr type 1` in log.
- [ ] **Live V/I/mode/power/temp/fan/runtime update** from the peer.  ← key gap

Control
- [ ] All 7 modes select; Set unit/label + default step follow the mode.
- [ ] Adjust ±/step chips/keypad set the setpoint exactly, clamped & rounded.
- [ ] Load ON/OFF tracks hardware state; blocked while faulted.

Resistance test
- [ ] Menu path + RT-mode path both run; Demo → R≈0.35 Ω, R²≈1.0.
- [ ] Progress/STOP work; STOP + finish leave the load OFF.
- [ ] Result rows correct; low-confidence banner on poor R².
- [ ] "Save to SD" — **known stub**, confirm no file yet (don't pass as real).

Graph
- [ ] Both traces auto-scale/scroll; range + window labels update.

Safety
- [ ] Fault banner latches, gates load-ON, clears on tap.
- [ ] Disconnect mid-test stops the load.
- [ ] Sweep never exceeds clamp limits; aborts out-of-range source.

---

*Keep this current as features land (SD save, bench tests, history, real-EL15
results). File issues against the specific `firmware/src/*` file + screen/state.*
