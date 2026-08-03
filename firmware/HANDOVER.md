# EL15 Controller Firmware — Session Handover

Living handover for the **standalone ESP32-C6 firmware** (`firmware/`) that turns
a Waveshare ESP32-C6-Touch-AMOLED-1.8 into an on-device controller for the
ALIENTEK EL15 electronic load. Update this at the end of each session.

**Last updated:** 2026-08-03.
**Branch:** `claude/android-apk-load-tester-k82q4g` (= `main`), at `b559d6b`.
`origin/main` and `origin/claude/…` are kept at the same commit, and pushing the
branch does NOT move main — push both.

---

## 0. Where things stand

**Proven on the real EL15:** connect, telemetry, all six mode opcodes, setpoint,
LOAD ON/OFF, SD read/write (including FAT timestamps and re-init after an
eject), the flash-backed datapoint log, and the **continuous R-test sweep — now
measured, not merely compiled**: 0.5 A / 10 s, repeats agreeing to 0.18–0.9 %,
with σ_R honest to within ~1.4× of the true run-to-run spread (§10).

**Also proven 2026-08-03 — the capacity test, with real current at last:** a
92 Ah lead-acid discharged at 4.6 A (0.05C) for 13 842 s. 17.68 Ah, 212 Wh,
SoH 19.2 %, pack resistance 12.6 mohm, stopped on its own at the 10.50 V cutoff.
The engine, the auto-save, the C-rate chips and the charge-state model all did
what they were built to do. The **SD card did not** — it lost 22 KB of the
report while reporting success (§16).

**Still never exercised:** pause/resume and the link guard against a genuine
unattended drop.

**Highest-value work next, in order:**

1. **Replace the SD card and re-verify** (§16). It silently dropped two 16 KB
   regions of a 300 KB file. Re-run `EL15_SDTEST` on the new one, and consider
   the read-back verify that §16 leaves open.
2. **Drill the safety layers** — link-guard hot drop, crash recovery, PWR
   long-press. None has fired for real. `FIRST_CONTACT.md` steps 15–16b.
3. **Finish the R-test sweep matrix** (duration 10/30/60 s × sample rate
   20/10/4 Hz). Cut short when the bench source collapsed — §10.
4. **Port the command checksum to the Android app.** `El15Protocol.kt` still
   emits checksum-less frames, so the app can read a real EL15 but cannot drive
   it. One helper plus five call sites.

### Session log

Detail is in `git log`; this is just the shape of things.

| Date | What happened |
|---|---|
| 2026-08-03 | **First capacity run with real current** — 92 Ah lead-acid, 4.6 A (0.05C), 13 842 s, 17.68 Ah, SoH 19.2 %, pack resistance 12.6 mohm. The engine, the C-rate chips and the charge-state model all worked. Its `BATT_007.CSV` came back with **22 KB of random bytes in two 16 KB-aligned regions — the SD card silently lost writes it had acknowledged** (§16). Added `tools/repair_report_csv.py`, and `saveCsv` now checks `sync()`/`close()` instead of discarding their result. |
| 2026-08-01 | **Probe wiring went device-wide.** 2-wire/4-wire + lead resistance moved out of the R-test into **Settings ▸ Probe wiring** and are now applied to every mode: `main.cpp compensateProbe()` adds `I × R_lead` back onto each status packet before it fans out. Biggest real effect is on the capacity test — its cutoff was firing at the load's terminals, so a 2-wire rig stopped early and under-reported the pack. The R-test is deliberately excluded (double-subtraction — §14). |
| 2026-08-01 | **Ten chemistries** (added LiPo HV, LTO, Na-ion, NiCd, alkaline; **lead-acid fixed at 12 V** — the only size this bench uses), each with its voltage range and a max cell count that keeps a full pack under 60 V. Picker rebuilt as one `lv_btnmatrix` after the chip version cost 6 KB of the BLE heap margin (§13). NVS battery keys bumped, since the chemistry indices moved. |
| 2026-08-01 | **Capacity test: real time remaining + C-rate from pack size.** New `battery_model.h` (per-chemistry OCV-vs-charge curves, standard test C-rates). The ETA now measures pack internal resistance from the switch-on sag, reads charge state off the curve, learns the pack's capacity during the run, and counts down to the CUTOFF — so it works with no rated capacity and no longer assumes a full pack. Setup gained C-rate chips that set the current from the rated capacity. `CAPACITY_PLAN.md` §4b/§4c. **Compile-clean only — no capacity run has ever drawn real current.** |
| 2026-08-01 | R-test rebuilt as a **continuous triangular sweep** with live graphs, replacing the stepped ladder — then four defects found by running it against real hardware (§9), and the ramp/sample timing tuned by measurement (§10). |
| 2026-08-01 | **Capacity overhaul**: pause/resume, rated-capacity metrics (C-rate, ETA, state of health), auto-save, flash-buffered per-sample CSV. **SD/RTC**: FAT directory timestamps, re-init after eject. Screen-timeout options. Fixed a running test being torn down by the user's own scan (§7). |
| 2026-08-01 | Documentation audit — every doc realigned with the code. |
| 2026-07-24 | **First real EL15.** Command checksums (§9), connect crash, control-write pacing, SD on bit-banged software SPI (§11), 20 Hz poll default (§10). |
| 2026-07-22 | Audio (ES8311, continuous-stream I²S), UI perf (QSPI 80 MHz), R-test accuracy work, on-device demo simulator removed in favour of the Android simulator app. |
| 2026-07-21 | Touch-snap engine, Settings screen, capacity test, physical buttons, engine mutual exclusion. Heap-corruption panic diagnosed (§7). |

---

## 1. Build / flash / monitor (read first)

PlatformIO Core is **off-PATH** at `~/.platformio/penv/Scripts/pio.exe`. The board
enumerates on a COM port that **hops between resets** (seen COM4 and COM7) — find
it with `"$PIO" device list` (or `[System.IO.Ports.SerialPort]::GetPortNames()`).
Bash + PowerShell both available.

```bash
PIO=~/.platformio/penv/Scripts/pio.exe
PORT=$("$PIO" device list | grep -oE 'COM[0-9]+' | head -1)
"$PIO" run -d firmware                                  # build (-Wall -Wextra on)
"$PIO" run -d firmware -t upload --upload-port "$PORT"  # flash
```
### ⚠ Opening a serial monitor RESETS this board

The USB-Serial/JTAG peripheral treats DTR/RTS transitions as a reset request, so
attaching a monitor reboots the firmware (`[boot] reset reason: USB`). That is
usually harmless and often what you want — but **never do it while the load may
be energised**. On 2026-08-01 a monitor attached during a live sweep rebooted the
controller with ~1 A flowing; the EL15 was left sinking with nothing commanding
it until its own protection tripped.

Rules that follow from that:
- A `-t upload` ends with a reset, so the firmware is **already running** by the
  time your next command opens the port. Anything that auto-starts on boot will
  be mid-run when you attach.
- To read serial WITHOUT resetting: open the port with `DtrEnable` and
  `RtsEnable` both `false` and do **not** pulse RTS. You lose the boot banner but
  the board keeps running.
- To read serial WITH a reset (to catch boot output), pulse RTS — but only once
  you know the load is off.
- Anything unattended that drives the load must wait for an explicit go signal
  rather than auto-starting. `EL15_RTUNE` does this (§12).

Read serial passively with a `System.IO.Ports.SerialPort` one-liner.
**If the board vanishes from USB entirely** (no COM port at all — happened once
under a very fast poll flood), physically unplug/replug the USB cable to recover.
Serial is 115200. Boot prints `[boot] reset reason: …` (power-on / PANIC / BROWNOUT / WDT / USB) —
use it when chasing spontaneous resets. `[audio] ready (ES8311)` confirms the
codec. Occasional `Wire.cpp requestFrom Error -1` are benign transient touch I²C
hiccups.

### Test scaffolding (build-flag gated, compiled out of normal builds)

| Flag | What it does |
|---|---|
| `EL15_SDTEST` | Boot-time SD info / write ×2 / readback + FAT stamp, plus a LittleFS datapoint-log exercise. **Draws no current.** |
| `EL15_RTUNE` | R-test tuning matrix — §12. **Draws current**; waits for a key press. |
| `EL15_POLLTEST` | Poll-rate sweep, fresh-vs-repeated frames per interval. |
| `EL15_SELFTEST` | Safe mode-cycle sweep (load stays OFF). |
| `EL15_NO_POLL` | Disable polling entirely — proves telemetry is poll-driven. |

```bash
PLATFORMIO_BUILD_FLAGS="-D EL15_SDTEST" "$PIO" run -d firmware -t upload --upload-port "$PORT"
```

**Clear `PLATFORMIO_BUILD_FLAGS` before building a shippable image.** It is an
environment variable, so it persists silently across commands in the same shell
and you will flash a test build without noticing. Verify from the boot log: a
harness announces itself, a clean build says nothing.

Framework is **Arduino** via the `pioarduino` platform (arduino-esp32 3.1.3).
Resolved lib versions: LVGL 8.4.0, Arduino_GFX 1.6.7, NimBLE-Arduino 2.5.0,
ESP_I2S 3.1.3, SdFat 2.x, LittleFS (bundled). A **stale** native-ESP-IDF build also exists in the tree
(`CMakeLists.txt`, `main/CMakeLists.txt`) — it predates the audio feature and
does **not** list `audio.cpp`/`es8311.c`, so it won't link until updated. The
PlatformIO/Arduino build is the one that works and is flashed.

---

## 2. Architecture & file map (`firmware/src/`)

```
main.cpp            owns objects, routes events, buttons, emergency stop
el15_protocol.h     wire protocol (header-only, pure): parse + command frames
el15_client.{h,cpp} BLE central (NimBLE 2.5): scan/connect/subscribe/poll/reassemble
el15_controller.h   El15Controller interface ONLY (demo simulator removed)
resistance_test.h   fuse-aware CONTINUOUS current sweep — triangular ramp, live
                    incremental fit, slope uncertainty, 4-wire/tare correction
capacity_test.h     battery discharge / capacity engine — also measures pack
                    internal resistance and estimates time-to-cutoff from charge state
battery_model.h     per-chemistry OCV-vs-charge-state curves + standard test
                    C-rates (header-only, pure); shared by the engine and the UI
display.{h,cpp}     CO5300/SH8601 AMOLED (QSPI 80 MHz) + touch + LVGL + touch-snap
                    engine + PMIC/RTC read+set + buttons + sleep + burn-in shift/dim
audio.{h,cpp}       ES8311 codec feedback (continuous-stream I2S tone synth)
es8311.{c,h},        vendored Espressif/Waveshare ES8311 driver (Arduino I²C HAL)
  es8311_reg.h
sd_card.{h,cpp}     microSD on bit-banged software SPI (SdFat); own driver (§12)
sample_log.{h,cpp}  per-datapoint capacity log buffered in flash (LittleFS),
                    tier-scheduled so a long run stays inside a fixed budget
report.h            CSV test reports (RTEST_/BATT_) written via sd_card
prefs.{h,cpp}       NVS persistence (debounced) + synchronous in-flight/creds flags
link_guard.h        link-loss auto-stop supervisor + crash-recovery (header-only)
netclock.{h,cpp}    Wi-Fi scan + NTP → PCF85063 (radio powered only per op)
ui.{cpp,h}          LVGL UI (~3100 lines) — all screens, overlays, result rows
board_config.h      ALL board pins (display, touch, PMIC, RTC, audio, buttons, SD)
include/lv_conf.h   LVGL config (fonts, chart, refr period 16 ms, indev 10 ms)
platformio.ini      pioarduino platform, qio_qspi, huge_app.csv, -Wall -Wextra
```

**Data flow:** NimBLE host-task callbacks only *enqueue* onto a FreeRTOS queue;
`El15Client::loopTick()` drains it on the loop task, so LVGL and the test engines
are only ever touched single-threaded on the loop task. `main.cpp handleStatus()`
fans a decoded status packet to `ui::onStatus()` + whichever engine is running.
**Rule:** never call LVGL or an engine from a NimBLE callback.

**Threading:** loop task (Arduino, prio 1) runs UI + BLE drain + engines. Audio
runs on its own FreeRTOS task at **prio 8** (above the loop) streaming I²S. NimBLE
host task is higher still.

---

## 3. Hardware facts (verified on this board — trust these)

Waveshare ESP32-C6-Touch-AMOLED-1.8. 368×448 portrait AMOLED.
- **No PSRAM.** 512 KB on-chip HP SRAM (~320 KB usable), 16 MB flash. LVGL uses a
  partial draw buffer; `BOARD_HAS_PSRAM` must NOT be defined.
- **Display:** Waveshare name it **CO5300**; driven with Arduino_GFX's
  `Arduino_SH8601` (compatible command set). QSPI at **80 MHz** (was 40).
  Reset/panel-enable via **TCA9554 expander (0x20) bits 4,5**. `LV_COLOR_16_SWAP 0`.
- **Touch:** Waveshare name **CST820**; this unit answers on the **FocalTech**
  (FT3168/FT6x36) register map. `touchInit()` writes both families' anti-sleep
  registers (FocalTech `0x86=0`, CST `0xFE=1`) so idle auto-sleep is defeated
  regardless. Coordinates clamped to panel bounds. I²C SDA=8 SCL=7 addr 0x38.
- **Audio:** **ES8311** codec at I²C **0x18** (shared bus). I²S MCLK=19 BCLK=20
  DIN=21 WS=22 DOUT=23. **Speaker amp power-enable = TCA9554 expander bit 7**
  (driven high in `audio::ampEnable()`).
- **PMIC:** AXP2101 at 0x34. VBAT ADC enabled at boot (reg 0x30 bit0). Battery %
  reg 0xA4, VBAT reg 0x34/0x35. **PWR key** arrives as PMIC IRQ bits (INTSTS2
  0x49, bits 3=short/2=long; enable INTEN2 0x41).
- **RTC:** PCF85063 at 0x51. Read via `rtcTime()`; now also settable via
  `setRtcTime()` (clears the oscillator-stop flag), driven by the NTP sync.
- **SD/TF slot:** SPI mode (C6 has no SDMMC), SCK=11 MOSI=10 MISO=18 **CS=6**,
  driven by a **bit-banged software-SPI** driver independent of the panel's SPI2
  bus — sharing that bus failed. See §12 and `sd_card.cpp`.
- **Buttons:** **BOOT = GPIO9** (active-low strapping pin). **PWR = PMIC key**.
- Unused hardware still on the board: QMI8658 IMU, 802.15.4 radio, RTC
  backup-battery pads. Wi-Fi is now used (NTP only). See `FEATURE_IDEAS.md`.

---

## 4. Feature set (current)

- **Connect:** scan (named EL15 devices only, dedup by address, random-address
  peers OK), connect, disconnect. Test WITHOUT hardware using the **Android
  simulator app** (`simulator/`) over real BLE — the on-device demo was removed.
- **Monitor:** V/I heroes (current turns red + "SINKING" when load on), telemetry
  row (W · fan% · temp · runtime), mode|set bar, pinned Load/RUN-TEST bar.
- **Adjust:** dial-stepper with hold-to-repeat + keypad; value card is the keypad
  button.
- **Graph:** live V/I two-series auto-scaling chart.
- **Mode picker:** CC/CV/CR/CP/CAP/DCR + RT + BATT pseudo-modes.
- **R-Test** (`SCR_RTEST`): fuse-aware bidirectional current sweep, V-I line chart
  (measured amber + fit green), full result rows incl. **Uncertainty (±)**,
  Voc, R², est. short-circuit I, sag, peak power, temp rise, max fan, and a
  **circuit-resistance estimator** (wire mm²/length, connections, fuse type →
  predicted R + residual vs measured). See `RTEST_ACCURACY.md`.
- **Battery capacity** (`SCR_BATT`): chemistry presets, cell-count with Voc
  auto-suggest, auto cutoff, CC discharge with local Ah/Wh integration, debounced
  cutoff + safety caps, rest/rebound, live discharge curve (continuous time
  axis), result rows. See `CAPACITY_PLAN.md`.
- **Settings:** brightness, **volume + mute**, sample rate (10/4/2/1 Hz), battery,
  clock, system info, restart.
- **Buttons:** **BOOT = hardware emergency stop** (kills load from any screen,
  red ack banner). **PWR = display sleep/wake** (true-black, touch inert).
- **Audio:** ES8311 tones — click on taps, PWR confirm, rising chime on test
  complete, falling on error, urgent alarm on fault/e-stop. Non-fatal init.
- **Touch-snap engine** (`display.cpp`): on each press finds the nearest real tap
  target within 40 px and shifts the gesture onto it — makes the small UI
  forgiving. Z-order/overlay aware; preserves scrolling.

---

## 5. Verified good / not yet verified

| Area | State |
|---|---|
| Build, clean under `-Wall -Wextra` | ✅ 2.18 MB / 3 MB slot, RAM 19.4 % static |
| Clean boot, no panic/bootloop; ES8311 init; buttons don't phantom-fire | ✅ |
| Audio tones clean (after the continuous-stream fix) | ✅ |
| BLE connect to a random-address peer (phone simulator) | ✅ |
| **Real EL15:** connect, live telemetry, all 6 mode opcodes, setpoint, LOAD ON/OFF | ✅ 2026-07-24 |
| **SD card:** mount, write ×2 with auto-increment, byte-correct readback | ✅ 2026-07-24 (via `EL15_SDTEST`) |
| R-test V-I chart full-width, battery graph time axis smooth | ✅ |
| **SD write path incl. FAT timestamps + re-init after eject** | ✅ 2026-08-01 (via `EL15_SDTEST`) |
| **Flash datapoint log** — mount, tier schedule, replay | ✅ 2026-08-01 |
| **R-test continuous sweep with real current** | ✅ 2026-08-01 — 0.5 A, 0.18–0.9 % run-to-run (§10) |
| **SD Save from the UI buttons** (automatic on completion) | ✅ 2026-08-03 — wrote BATT_007.CSV (the CARD then lost 22 KB of it, §16) |
| **Capacity test with real current** | ✅ 2026-08-03 — 92 Ah lead-acid, 4.6 A, 17.68 Ah, SoH 19.2 % |
| Rated-capacity metrics, running-test chip | ✅ 2026-08-03 | 
| Pause/resume | ❌ compile-clean, still never exercised |
| **Charge-state time estimate, pack-resistance measurement, C-rate chips** | ✅ 2026-08-03 — all three exercised on the 92 Ah run |
| Link guard, crash recovery, brownout auto-off, load-safe power-off | ❌ never fired for real |
| NVS persistence, burn-in shift/dim, NTP sync, Kelvin + tare | ❌ compile-clean only |

**Needs a human's eyes/ears (couldn't verify remotely):**
- Display integrity at **80 MHz** QSPI — user hasn't reported artifacts, but
  confirm no garbling/wrong colors. Fallback: 64 MHz or back to 40 in
  `display.cpp` `g_gfx->begin(...)`.
- Audio **latency** (inherent DMA depth) and any faint idle hiss in the 2.5 s
  keep-alive window.
- Full R-test / capacity runs against the **phone simulator or a real EL15**
  (no on-device demo now, so these can't be auto-driven from firmware).
- **Real ALIENTEK EL15**: load control, all modes, telemetry and now a full
  **R-test sweep with real current** are VERIFIED (§9, §10). A **capacity run
  with real current is still completely unproven**.
- **SD card**: read+write verified on hardware via the test scaffolding (§11);
  the **UI Save path is still untested** — run Settings ▸ SD ▸ Check card, then an
  R-test/battery Save, and confirm `RTEST_NNN.CSV` opens in a spreadsheet and that
  with no card the button honestly says "No card detected".
- **Persistence / burn-in / NTP / Kelvin (this session):** all compile and the
  board boots clean, but none is confirmed working end-to-end. To verify:
  change brightness/volume/sample-rate → reboot → they stick; leave it idle and
  watch the dim→blank, tap to wake; Settings ▸ Clock → tap "Wi-Fi network" to
  scan, pick one, type the password → "Sync clock now" sets the RTC row;
  R-test ▸ 2-wire ▸ "Measure (short the probes)"
  stores a tare that then subtracts from a real run; toggle 4-wire and confirm
  the result shows the wiring. Then re-run an R-test/capacity save and confirm
  the CSV header now carries the probe wiring + timestamp.
- **Link-loss / crash recovery:** kill the simulator mid-discharge → expect the
  red locked banner + repeating alarm + reconnect attempts; pull power mid-test
  → on reboot expect the amber "LOAD MAY STILL BE ON" recovery offer.

---

## 6. Open items & next steps (prioritized)

**Do these first — validate what already exists**
- **Run a CAPACITY test with real current.** The R-test has now been proven with
  real current (§10); the capacity engine has not, in any form — pause/resume,
  rated-capacity metrics, auto-save and the per-sample CSV are all unexercised.
  A small protected cell with the cutoff set high is enough.
  `FIRST_CONTACT.md` Phase 2, steps 19–23.
- **Exercise the SD Save buttons.** Settings ▸ SD ▸ Check card, then an
  R-test/battery Save. The path underneath is verified; the buttons are not.
  Delete the leftover `SDTEST_00N.CSV` files while you're there.
- **Drill the safety layers** (link-guard hot drop, crash recovery, PWR
  long-press) — `FIRST_CONTACT.md` steps 15–16b. None has ever fired for real.
- **Finish the R-test tuning matrix** (§10, §12). The duration and sample-rate
  arms are still unmeasured — the bench source collapsed part-way through.

**Loose ends from this session**
- **The in-flight recovery flag stuck as set across several boots** even after
  the guard reported clearing it, so every boot offered recovery. It stopped
  reproducing once instrumented and is currently clean. `prefs::armInFlight()`
  now logs each transition and whether the NVS write took —
  `[prefs] inFlight -> N (NVS WRITE FAILED)` is the smoking gun to watch for.
- **The bench source collapsed mid-sweep once** (20 V → ~0 V at 0.5 A, self
  recovering). Unexplained. The harness's 10 %-of-Voc sag guard caught it.

**Safety / correctness**
- **Port the command checksum to the Android app.** `El15Protocol.kt` still
  builds every command frame without the trailing sum-to-zero byte, so the app
  can read a real EL15 but cannot control it — the same bug this firmware had
  until 2026-07-24. One `frameChecksum()` helper plus five call sites.
- ~~Triage the rest of the QA audit~~ **done 2026-08-01** — re-read against
  `6adea41` and recorded in `QA_REPORT.md`. 10 of 12 H/M findings are fixed, M1
  was a deliberate semantics decision (documented), and **M3 is still open**: a
  scan window that simply expires never resets the state, so the UI keeps saying
  "Scanning". Open low-severity leftovers: L1 (fuse keypad unreachable), L4 (DCR
  hero shows `dcrI1`), L6 (progress callbacks yank you back to the R-test
  screen), L8 (informative connect-failure text overwritten by "Disconnected").
- ~~BLE-drop supervisor during a capacity discharge~~ **done** — `link_guard.h`
  now reconnects and force-pushes LOAD OFF on any hot link loss, with a locked
  alarm banner, and an NVS in-flight flag offers the same recovery after a
  crash/reboot. Still recommend setting the EL15's hardware UVP as a backstop
  (the guard needs a working radio to act). **Untested against a real drop** —
  verify by killing the simulator mid-discharge.

**Biggest felt improvements**
- **Async `esp_lcd` DMA flush** — the real fix for UI lag. Arduino exposes
  `esp_lcd`, so it can be done in-place without leaving the Arduino build:
  replace the synchronous `draw16bitRGBBitmap` flush with an async panel + a
  done-callback calling `lv_disp_flush_ready`, restore double buffering. Roughly
  halves redraw cost. (User asked about Arduino vs IDF — conclusion: stay
  Arduino, drop to IDF APIs only here and for I²S if needed.)
- **Free flush throughput: drop `is_shared_interface`.** `display.cpp` still
  builds the panel bus in shared mode, which acquires/releases the SPI lock on
  every flush chunk. Its only reason was the SD card sharing SPI2, which stopped
  being true on 2026-07-24. Setting it `false` is a one-word change worth a few
  percent per redraw — but it changes how the panel is driven, so verify on
  hardware (garbling / wrong colors) before keeping it.
- ~~SD card save~~ **done + verified on hardware** (2026-07-24): rewritten onto
  bit-banged software SPI via SdFat (§12) after the shared-bus scheme proved
  impossible. `sd_card.cpp` + `report.h` write real `RTEST_NNN.CSV` /
  `BATT_NNN.CSV`; read/write proven end-to-end. Only the UI Save button path
  remains to be exercised.
- ~~NVS persistence~~ **done** — `prefs.cpp` persists brightness, volume/mute,
  sample rate, screen-protection, R-test + battery setup, Wi-Fi creds and the
  last device; debounced commit from `loop()`, restored in `ui::begin()` before
  the widgets are built. Named/recallable profiles are the remaining stretch.
- ~~RTC set-time~~ **done via NTP** — Settings ▸ Clock: enter Wi-Fi + UTC
  offset, "Sync clock now" (`netclock.cpp`) sets the PCF85063. A manual
  stepper set-time UI is still a nice-to-have for when there's no Wi-Fi.

**R-Test accuracy tier 2** (RTEST_ACCURACY.md §5, items 4–6, not done)
- Tare/zero step (subtract lead+contact resistance) — biggest low-R absolute-
  accuracy win. Curvature/residual flag. Average the priming Voc.

**Feature backlog:** see `FEATURE_IDEAS.md` (audio §14 largely done; buttons §15
partly done — hardware e-stop + sleep done, start/stop/screenshot not).

---

## 7. Gotchas & lessons (don't relearn these)

- **A non-CONNECTED BLE state is NOT a link drop.** `main.cpp`'s state handler
  used to tear down any running test whenever the state wasn't `CONNECTED` — but
  `SCANNING` and `CONNECTING` aren't either, so the user opening Connect and
  tapping **Scan** mid-discharge silently killed the test, and mid-priming it
  died as "Cancelled" leaving nothing to save. Latch `g_wasConnected` and act
  only on a real drop from a live link. The link guard keeps its own copy of the
  same latch for the same reason.
- **A stale protection must not veto the next test.** Priming used to abort the
  instant it saw a warning bit, so a trip left over from a link drop or a
  controller reset made the R-test unusable. Priming already commands the exact
  state that lets the device shed a trip (CC / setpoint 0 / LOAD OFF), so it now
  keeps pushing that for a 4 s grace period. A trip *during* a sweep still aborts
  immediately — current is flowing then.
- **Heap-corruption panic (2026-07-21):** rebuilding LVGL result rows per test
  (`lv_obj_clean` + chart `set_point_count` realloc) interleaved frees with chart
  buffer reallocs → Load-access-fault in the next layout pass. **Fix pattern kept
  everywhere:** result rows and chart point counts are allocated ONCE and only
  text-/value-updated. Preserve this; don't reintroduce per-test allocation.
- **LVGL scatter charts** mis-render unused/`POINT_NONE` slots (stray edge line).
  Use LINE charts + resample-to-full-width (battery + R-test both do this now).
- **Audio must stream continuously** — stopping/restarting the I²S write per tone
  glitches. Keep the one continuous-stream task in `audio.cpp`.
- **UI flush is synchronous** and partly CPU-bound in Arduino_GFX; buffer/clock
  tuning hit its ceiling. Async `esp_lcd` is the real lever.
- **Two engines, one load:** R-test and capacity must be mutually exclusive and
  manual load/setpoint controls gated while either runs (`engineBusy()` in
  ui.cpp + guards in main.cpp). Keep these guards.
- **Board pin/chip names** in Waveshare's marketing (CO5300/CST820) differ from
  what the code drives (SH8601/FocalTech) — both documented in `board_config.h`.
- **Wi-Fi and BLE share one antenna path** on the C6. NTP powers the radio only
  for the sync and turns it off again (`netclock.cpp` always `radioOff()`s), and
  a sync is refused while a test runs — the BLE link is the only way to stop the
  load, so it must not be starved. Keep both rules if you add more Wi-Fi.
- **RAM is the binding constraint — it gates BOTH BLE and Wi-Fi.** 320 KB, no
  PSRAM, and the LVGL UI allocates from the system heap (`LV_MEM_CUSTOM`). This
  session's UI growth (prefs, Wi-Fi/keyboard overlays, extra Settings cards) with
  a 1/4-frame (112-line, 82 KB) draw buffer left only ~13 KB free — which starved
  **NimBLE's connection establishment**, so every BLE connect failed with HCI
  0x3e (looked like "connect failed"; scanning still worked). Fix: the draw
  buffer is now **1/7 (64 lines, ~47 KB)** — see `BUF_LINES` in display.cpp —
  freeing ~35 KB, bringing free heap to ~37 KB and letting connects succeed.
  Keep a **≥~30 KB contiguous margin for BLE**; if you grow the UI or the buffer,
  re-check that connects still work. (First-principles proof it was memory:
  freeing heap right before a connect made it succeed instantly.)
  **You no longer have to guess at this margin:** `main.cpp` prints
  `[boot] heap after UI: N B free, largest block N B` once the UI has finished
  allocating. Read that line after any screen grows. It is also how the chemistry
  picker ended up as a single `lv_btnmatrix` rather than ten chip widgets —
  measured 31.7 KB vs 37.9 KB largest block (§13).
- **Wi-Fi needs even more (~50 KB contiguous) than the trimmed buffer leaves**,
  so `display::setLowMemMode(true)` shrinks the draw buffer to 16 lines for the
  duration of a scan/sync, freeing ~70 KB; the UI keeps rendering (more, smaller
  flush chunks). The full buffer can NOT be reassembled afterwards (fragmentation
  ~18-40 KB max), so a successful **clock sync auto-reboots** to get it back
  (RTC + NVS persist, nothing lost). Wrap any new Wi-Fi feature in the same
  low-mem window. Do NOT try to raise the draw buffer back to full at runtime.
- **Reclaiming buffer size = trim baseline heap.** If the UI is too slow at 1/7,
  the way back to a taller buffer is to lazy-build + destroy the rarely-used
  Wi-Fi/keyboard overlays (they sit resident today), then grow `BUF_LINES` while
  keeping the BLE margin above.
- **Loop stack is 12 KB** (`ARDUINO_LOOP_STACK_SIZE`), raised from 8 KB for
  FATFS's on-stack long-filename buffer. NVS, Wi-Fi and the SD writes all run on
  the loop task; don't drop it back.
- **NVS commits are debounced** (`prefs::tick()`, 1.5 s settle) so a slider drag
  is one flash write, but the in-flight recovery flag and Wi-Fi creds are
  written synchronously on purpose — they have to survive the very next event.
- **One SPI host, and the panel owns it.** The C6 has a single general-purpose
  SPI controller (SPI2) and the AMOLED holds it in QSPI mode. Making the SD card
  a second *device* on that host — swinging the signals between two pin sets
  through the GPIO matrix per access — **was tried and does not work**: the IDF
  `sdspi` driver can't transact on the panel's bus (card init dies at CMD59).
  That scheme is deleted. The card runs on **bit-banged software SPI** on its own
  pins (§12), so a redraw and a card access no longer interact at all. Two
  leftovers from the old scheme: `Arduino_GFX` is still constructed with
  `is_shared_interface = true` (now unnecessary, costs a lock round-trip per
  flush chunk — see §7), and the UI still paints its "Saving..." state with
  `lv_refr_now()` before calling in, which is still correct because the save
  **blocks the loop task** for ~1 s even though it no longer blocks the bus.

---

## 8. Companion docs

All refreshed 2026-08-01 against `6adea41`.

- `README.md` — build/flash, feature set, hardware notes, architecture.
- `FIRST_CONTACT.md` — ordered real-EL15 bench checklist. **Phases 0–1 done;
  start at step 15.**
- `QA_GUIDE.md` — per-feature test matrix & procedures, wire protocol, safety
  behavior, known gaps.
- `QA_REPORT.md` — the 2026-07-21 code audit, now with a resolution table for
  every finding plus three new ones.
- `RTEST_ACCURACY.md` — R-test methodology; items 1–4 implemented, 5–6 open.
- `CAPACITY_PLAN.md` — battery-test roadmap; phases 0/1/2/4 done, 3 (per-sample
  CSV streaming) open.
- `FEATURE_IDEAS.md` — full feature/UX/audio/buttons backlog, with what's landed
  struck through.
- `UI_DESIGN_BRIEF.md` — v2 "Focus" UI spec. A **design-time brief**, written
  before the hardware was explored: it predates the physical buttons, audio,
  Settings, and the battery screen, and its §1 still mentions a built-in demo
  simulator. Read it as the visual language, not as a feature list.

---

## 9. EL15 wire protocol on real hardware (2026-07-24)

The protocol is confirmed against a real unit now, not just the simulator.

- **Frame format:** `AF 07 03 <cmd> <len> <data…> <checksum>`. The checksum is the
  byte that makes the whole frame sum to **0 mod 256** — the same rule the status
  parser enforces on incoming packets. `el15_protocol.h frameChecksum()` computes
  it; `LOAD_ON/OFF/LOCK` carry it baked in, `modeCommand()/setpointCommand()`
  append it. Do NOT send a command without it — the device silently drops it.
- **GATT:** service FFF0, notify FFF1 (`--wN-`), **command char FFF3 (`--w--`,
  write-no-response ONLY)**. The per-connect dump logs this. `writeRaw` uses
  no-response because that's all FFF3 offers.
- **Telemetry is POLL-driven:** the device sends a status frame only in reply to a
  POLL write; it does not free-run. (Proven by disabling polling — all telemetry
  stopped.) So a working POLL is proof FFF3 writes land.
- **Control writes must be paced** (§9): ≥50 ms apart and clear of the poll,
  or the device drops the one that arrives too soon. `writeRaw` enforces this.
- **All 6 mode opcodes verified** on hardware (CC 01, CV 09, CR 11, CP 19,
  CAP 02, DCR 0A) — commanded mode == device-reported `b5`.
- **LOAD_ON at a 0.000 A CC setpoint does nothing.** The write is accepted
  (`write OK`) but the device reports `load=0` and sinks no current, and it never
  recovers on its own — a sweep starting from 0 A ran to completion with `I=0`
  throughout and no usable data. **Always command a non-zero current before
  LOAD_ON.** `resistance_test.h commandable()` floors the ramp at
  `MIN_TEST_CURRENT` (0.05 A) for this reason, and the engine re-asserts LOAD_ON
  (rate-limited to 1 Hz) if a mid-sweep packet ever reports the load off. The old
  stepped ladder never hit this because its first level was `maxCurrent/n`,
  never zero. Found 2026-08-01 on real hardware.

## 10. Sample-rate and sweep tuning (measured)

### Poll rate (2026-07-24)

Swept 250→20 ms polling, comparing full 28-byte frames for fresh-vs-repeated:

| poll | rx Hz | fresh Hz | % unique |
|---|---|---|---|
| 100 ms | 10.0 | 10.0 | 100% |
| 75 ms | 13.1 | 12.9 | 97% |
| **50 ms** | 19.7 | **17.4** | 88% |
| 33 ms | 27.1 | 16.8 | 62% |
| 20 ms | 40.8 | 16.6 | 40% |

The EL15 produces **~17-19 fresh samples/s** (min ~23 ms between distinct frames).
**50 ms (20 Hz) is the practical max** — it captures ~all the fresh data; faster
just refetches repeats and floods the link (and a 20 ms flood once dropped the
board off USB). **50 ms is now the default** (`prefs`, and the Settings chips are
`20/10/4/2 Hz`). The NVS key was bumped `pollMs`→`pollMs20` so existing devices
re-default cleanly.

### Control-write vs poll timing (2026-08-01)

A control write used to reset `lastPollMs_`, pushing the next poll a **whole poll
interval** away. During an R-test sweep — which re-commands the setpoint at
10 Hz — that halved the sample rate: a 20 Hz poll delivered ~12 Hz of data.
`El15Client::ctrlPollGapMs` now holds the poll off by a short gap instead, so the
two cadences are independent. Two *control* writes still need their 50 ms
(that is the separation the device enforces by dropping the second).

Three-way A/B on real hardware, 0.5 A over 10 s, all in one session so the DUT
could not drift between arms:

| gap | samples / 10 s | σ_R | R² | run-to-run spread |
|---|---|---|---|---|
| **25 ms** | **150, 150** | 0.95 mΩ | 0.979 | 0.18 % |
| 40 ms | 143, 120 | 0.82 mΩ | 0.986 | 1.46 % |
| 60 ms | 95, 87 | 0.97 mΩ | 0.987 | 0.24 % |

**σ_R is flat across all three** — the tighter gap costs nothing in fitted
uncertainty and buys 70 % more samples. The extra points are individually
noisier (R² 0.979 vs 0.987) by almost exactly the factor the larger *n*
compensates for, which is why σ lands in the same place. **Default 25 ms.**

Do not chase a true 20 Hz during a sweep: ~15 Hz is near the ceiling, because the
device only produces ~17–19 fresh samples/s and a sweep also carries a 10 Hz
command stream.

The 40 ms arm's poor spread was **not** the gap — one of its runs lost the first
second of the ramp because the initial LOAD_ON did not land (§10) and the
re-assert interval was 1 s. That is now 400 ms.

### R-test repeatability (measured)

0.5 A / 10 s / 20 Hz, three repeats: **R = 84.15 mΩ, run-to-run spread 0.61 mΩ
(0.72 %)**, R² 0.995–0.998, ΔT under 1.1 °C. The fit's self-reported σ_R averaged
0.45 mΩ against that 0.61 mΩ spread — the right order of magnitude, mildly
**optimistic**. Worth knowing, since the `reliable` flag is gated on σ_R.

Two cautions for anyone repeating this:
- **Absolute R is not comparable across sessions.** The same physical setup read
  53.6 mΩ, then 84.2 mΩ, then 77.4 mΩ across this session's reflashes. Contact
  resistance moves. Compare *within* a session, or use the tare/4-wire workflow.
- **The bench source collapsed mid-run once** (20 V → ~0 V during a 20 s sweep at
  0.5 A, recovering on its own afterwards). The harness aborts if sag exceeds
  10 % of Voc, which is what caught it. Cause unknown; the duration/sample-rate
  matrix is still unfinished because of it.

## 11. SD card — bit-banged software SPI (2026-07-24)

**The C6 has ONE general-purpose SPI host and the AMOLED owns it in QSPI mode.**
The old "share the bus via GPIO-matrix reroute" scheme (now deleted) never worked:
the IDF `sdspi` driver can't transact on the panel's bus — card init dies at CMD59
(`ESP_ERR_NOT_SUPPORTED`). So the card runs entirely in **software SPI** on its
dedicated pins (SCK 11 / MOSI 10 / MISO 18 / CS 6), independent of SPI2 — a screen
redraw and a card access no longer interfere at all.

Implementation (`sd_card.cpp`, via **SdFat**):
- A small custom `SdSpiBaseClass` driver (`SPI_DRIVER_SELECT=3`) that bit-bangs
  with plain `digitalWrite/digitalRead` — their ~1 µs overhead gives an inherently
  slow, reliable clock (~250 kHz). SdFat's built-in `SoftSpiDriver` uses fast
  register GPIO that was too fast → 512-byte data-block writes corrupted.
- **`USE_SD_CRC=1`** — the card enforces command CRCs; without real CRC7 SdFat
  sends a fixed bogus byte and ACMD41 is rejected (`R1=0x08`). Also protects data.
- **`SHARED_SPI`** (not DEDICATED) — a DEDICATED multi-block write was left
  un-terminated over the link, failing the next file open. SHARED cycles CS per op.
- **Mount once, stay mounted** + init retries — re-running card init over soft SPI
  is flaky; the card is on dedicated pins so there's no reason to unmount.
- `report.h` writes via SdFat's `Print` (`fpf()` printf helper), not `FILE*`.
- Build flags: `SPI_DRIVER_SELECT=3`, `USE_SD_CRC=1`, lib `greiman/SdFat`.

Verified end-to-end: mount → two writes with incrementing `SDTEST_NNN.CSV` →
byte-correct readback (with real RTC timestamp) → card healthy. A **wedged card**
(from an aborted write) reports CMD0 `R1=0x00` and only a **physical reseat /
power-cycle** clears it — an ESP reset won't.

**Still untested (as of 2026-08-01):** the actual UI Save path — Settings ▸ SD ▸
Check card, and the R-test/battery **Save** buttons. Exercise it once and confirm
the CSV opens in a spreadsheet with a real RTC timestamp and the probe wiring in
its header. Delete the `SDTEST_00N.CSV` files left from bring-up.

---

## 12. The R-test tuning harness (`EL15_RTUNE`)

In `main.cpp`, gated like the other scaffolding. Runs a matrix of sweeps
back-to-back and prints one CSV row per run, then a summary.

**It reports run-to-run SPREAD, not just the σ each fit claims about itself** —
a fit can be confidently wrong, and only repetition shows it. Every config runs
`REPEATS` times.

```bash
PLATFORMIO_BUILD_FLAGS="-D EL15_RTUNE -D RTUNE_PEAK_A=0.5f" "$PIO" run -d firmware -t upload --upload-port "$PORT"
# add -D EL15_RTUNE_STAGE2 for the duration/rate matrix
# add -D EL15_RTUNE_STAGE3 for the ctrl->poll gap matrix
```

Safety properties, all of which exist because something went wrong without them:
- **Never auto-starts.** Connects, prints `ready - press any key`, waits. Every
  reset your monitoring causes therefore happens before anything is energised
  (§1).
- **Forces LOAD OFF on boot** if the in-flight flag is set, instead of waiting
  for a banner tap — a headless rig has nobody to tap.
- **Aborts if sag exceeds 10 % of Voc**, i.e. if the source is not stiff enough
  for the current being asked of it.
- `RTUNE_PEAK_A` is deliberately far below what the fuse/ratings would allow.
  Accuracy tuning is about repeatability, not amplitude.

Driving it from the host: open the port **without** pulsing RTS, wait for the
ready line, write one byte, then capture. There is a worked example in this
session's git history.

---

## 13. The capacity test's battery model (2026-08-01)

`battery_model.h` holds a per-cell open-circuit-voltage curve for each chemistry
plus the C-rates that chemistry is conventionally tested at. Ten chemistries:
Li-ion, LiPo HV (4.35 V), LiFePO4, LTO, Na-ion, **lead-acid fixed at 12 V**,
NiMH, NiCd, alkaline (primary), Custom. Every max cell count is chosen so a
FULLY CHARGED pack of that size stays under the EL15's 60 V input rating.
Two features hang off it; full rationale in `CAPACITY_PLAN.md` §4b/§4c/§4d.
What matters here:

- **The old ETA was wrong twice over** — it assumed the pack started full, and it
  counted down to the *rating* when the *cutoff* is what ends the run. The new one
  reads charge state off the curve and targets the cutoff.
- **A loaded voltage cannot be looked up on a rested curve.** It must be referred
  back through I·R first. R (pack + contacts + leads) is measured from the
  switch-on sag against priming's open-circuit reading, over active discharge
  seconds 1.5–5, sampled only while the load is regulating, with a 3-sample
  quorum. Get this wrong and every charge figure is wrong: 2 A through 150 mΩ is
  0.3 V, most of the gap between 40 % and 70 % on Li-ion.
- **Use the same R at both ends.** The cutoff is compared against the *loaded*
  voltage, so the charge state it corresponds to is the one at `cutoff + I·R`.
  Because both the reading and the target carry the same R, lead resistance
  shifts them together instead of biasing the time between them. Do not
  "improve" one end without the other.
- **Capacity is learned, not trusted.** After 10 % of charge travel,
  `Q = Ah drawn / charge travelled`. That is what lets an ETA exist with no
  rated capacity entered, and what stops the estimate depending on the curve's
  absolute calibration. A nameplate rating only *bounds* the learned value
  (0.05×…5×) — it never overrides it, because measuring how wrong the nameplate
  is happens to be the point of the test.
- **The filter is time-based, not per-sample.** The sample rate is a user setting
  spanning 2–20 Hz; a fixed per-sample coefficient would make the charge-state
  filter ten times slower at the bottom of that range.
- **Fallbacks are labelled, never silent.** Custom chemistry has no curve, and a
  run where R was never measurable never invents one. Both drop to the old
  rated-capacity estimate and the UI says "(from the rating)".
- **Chemistry indices are load-bearing and they moved.** The presets are stored
  in NVS as an INDEX, so growing the list renamed everybody's saved chemistry.
  The four battery-setup keys were therefore bumped (`btChem`→`btChem2`, and the
  same for cells/cutoff/cutoff-custom) so the whole setup re-defaults once
  together — otherwise a device came back as LiFePO4 carrying an old lead-acid
  42 V cutoff. Same trick as `pollMs`→`pollMs20`. **Do this again if the list
  order ever changes.**
- **The picker is ONE `lv_btnmatrix`, and that is a heap decision.** Ten chip
  buttons with ten labels cost ~6 KB and dropped the largest contiguous block to
  31.7 KB — 1.7 KB above what NimBLE needs to connect. The matrix renders the
  same grid as a single object: 37.9 KB. `main.cpp` now prints
  `[boot] heap after UI: … free, largest block …` so this is observable on any
  boot. **Watch that line whenever a screen grows.** (Checked before committing
  to it: `snapOffset()` clamps a near-miss INTO the target rect and leaves an
  inside-the-target point alone, so touch-snap cannot displace a tap onto a
  neighbouring button of the matrix.)
- **The curves are per family, not per cell.** Every charge figure in the UI is
  prefixed "~" for that reason. LiFePO4's plateau spans ~150 mV from 20 % to
  90 %, so its charge readout is coarse until the knee — the learned-capacity
  term carries it there. Do not present any of this as a measured state of
  charge.

## 14. Probe wiring is device-wide (2026-08-01)

`Settings ▸ Probe wiring` holds 2-wire/4-wire and the lead resistance for the
WHOLE controller. `main.cpp compensateProbe()` applies it to every status packet
before the packet fans out. Things to know before touching it:

- **One correction, one place.** The compensation happens once, in
  `handleStatus()`, so the UI and the engines can never disagree about what the
  voltage is. Do not add a second correction downstream.
- **The R-test gets the RAW packet, deliberately.** Its fit already removes the
  tare from the SLOPE (`resistance_test.h correct()`), so a pre-corrected voltage
  would subtract the same resistance twice and a low-milliohm result could come
  out at zero. If you ever unify the two, remove one of them — not neither, and
  not both.
- **The tare sweep must stay uncorrected too.** It measures the leads; feeding it
  lead-corrected volts would measure zero. It is safe today only because it runs
  through the R-test path, which is already excluded. Keep that true.
- **`setpoint` is not corrected.** In CV mode it is a command the device
  regulates at its own terminals, not a measurement. The gap between it and the
  corrected reading IS the lead drop, which is useful to see.
- **Biggest practical win is the capacity test**, not the R-test: the cutoff used
  to be applied at the load's terminals, so a 2-wire discharge stopped early by
  the lead drop and under-reported the pack. It now cuts at the pack.
- **Knock-on:** the capacity test's internal-resistance figure measures the pack
  ALONE once compensation is active (the leads are already out of the reading),
  so the result row relabels itself "Pack resistance" vs "Pack + lead
  resistance". `BATT_NNN.CSV` records the wiring and whether voltages were
  corrected — without that the file's volts would be ambiguous.
- **Both screens edit the same two values** (`probeFourWire`, `probeTareOhm`);
  the R-Test card is where the tare is MEASURED, not a second setting. Any edit
  must call `persistProbe()` — main.cpp reads `prefs` on every packet, so a value
  that only reaches prefs on the next screen refresh would be silently stale.

## 15. Doc map — read in this order

New to the tree? `README.md` (what it is and how to build) → this file §0/§3/§7
(state, hardware facts, gotchas) → `FIRST_CONTACT.md` (what to do at the bench).
`QA_GUIDE.md` is the reference you come back to per feature.

*Everything is committed and merged to `main`.*

## 16. The SD card silently loses writes (2026-08-03)

`BATT_007.CSV` — the first real-current capacity run, 317 627 B — came off the
card with two regions of pure-random bytes:

| file offset | length | aligned to |
|---|---|---|
| 114688 – 131071 | 16384 B (exactly 16 KB) | 16 KB, offset 7 |
| 311296 – EOF | 6331 B | 16 KB, offset 19 |

**It is the card, not the firmware.** The chain of evidence:

- Both regions are aligned to, and sized in, the card's **16 KB allocation
  unit**. Nothing in the firmware has 16 KB granularity — SdFat's cache is 512 B,
  the sample-log batch is 768 B, `fpf()`'s buffer is 192 B.
- The byte accounting across each gap is **exact**: the missing bytes are
  precisely the rows that belong at those offsets (289 rows at t=4028–4604, and
  the tail from t=13428). So the firmware generated and submitted every byte.
- **`USE_SD_CRC=1`.** A corrupted SPI transfer would have been rejected by the
  card and surfaced as a write error, and `report.h` checks `getWriteError()`
  after every row — the replay would have stopped and `saveCsv` would have
  DELETED the file rather than saving it. None of that happened, so the card
  acknowledged every write.
- The garbage is uniformly random — 7.99 bits/byte, no repeated 16-byte blocks,
  no recoverable text. Recycled deleted-file data has structure; erased flash is
  0x00/0xFF. Random is what an FTL returns for a page it never actually
  programmed.

So the card took ~22 KB of writes, said OK, and did not retain them. Treat this
card as suspect — it is the same one that was "verified" during bring-up, but
that test wrote ~200-byte files; this was the first time anything asked it for
300 KB. **Replace it before the next long run**, and re-run `EL15_SDTEST`.

- `tools/repair_report_csv.py` strips the garbage from a damaged report, keeps
  every intact row and marks the holes. It recovered 93 % of BATT_007.
- The **summary block is computed by the instrument, not from the datapoint
  rows**, so capacity/energy/duration/SoH survived intact. That separation is
  worth keeping.
- **Still open:** nothing verifies a report after writing it. A read-back +
  compare is the only thing that would have caught this at the time, and it
  roughly doubles save duration (~10 s → ~20 s for a file this size). Not
  implemented — it is a deliberate cost/benefit call, not an oversight.
