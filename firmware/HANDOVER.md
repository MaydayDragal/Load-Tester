# EL15 Controller Firmware — Session Handover

Living handover for the **standalone ESP32-C6 firmware** (`firmware/`) that turns
a Waveshare ESP32-C6-Touch-AMOLED-1.8 into an on-device controller for the
ALIENTEK EL15 electronic load. Update this at the end of each session.

**Last updated:** 2026-07-22.
**Branch:** `claude/android-apk-load-tester-k82q4g` — pushed, working tree clean,
in sync with `origin`. Tip commit `0ab80e8`.

---

## 1. Build / flash / monitor (read first)

PlatformIO Core is **off-PATH** at `~/.platformio/penv/Scripts/pio.exe`. Board
enumerates as **COM4**. Bash + PowerShell both available.

```bash
PIO=~/.platformio/penv/Scripts/pio.exe
"$PIO" run -d firmware                                 # build (-Wall -Wextra on)
"$PIO" run -d firmware -t upload --upload-port COM4    # flash
```
Serial is 115200. Read serial from PowerShell (the `pio device monitor` holds
the port); a `System.IO.Ports.SerialPort` one-liner works well for short grabs.
Boot prints `[boot] reset reason: …` (power-on / PANIC / BROWNOUT / WDT / USB) —
use it when chasing spontaneous resets. `[audio] ready (ES8311)` confirms the
codec. Occasional `Wire.cpp requestFrom Error -1` are benign transient touch I²C
hiccups.

Framework is **Arduino** via the `pioarduino` platform (arduino-esp32 3.1.3).
Resolved lib versions: LVGL 8.4.0, Arduino_GFX 1.6.7, NimBLE-Arduino 2.5.0,
ESP_I2S 3.1.3. A **stale** native-ESP-IDF build also exists in the tree
(`CMakeLists.txt`, `main/CMakeLists.txt`) — it predates the audio feature and
does **not** list `audio.cpp`/`es8311.c`, so it won't link until updated. The
PlatformIO/Arduino build is the one that works and is flashed.

---

## 2. Architecture & file map (`firmware/src/`)

```
main.cpp            owns objects, routes events, buttons, emergency stop
el15_protocol.h     wire protocol (header-only, pure): parse + command frames
el15_client.{h,cpp} BLE central (NimBLE 2.5): scan/connect/subscribe/poll/reassemble
el15_controller.h   El15Controller interface ONLY (demo simulator removed — see §5)
resistance_test.h   fuse-aware sweep engine — bidirectional + slope uncertainty (§6)
capacity_test.h     battery discharge / capacity engine
display.{h,cpp}     CO5300/SH8601 AMOLED (QSPI 80 MHz) + touch + LVGL + touch-snap
                    engine + PMIC/RTC reads + buttons + display sleep
audio.{h,cpp}       ES8311 codec feedback (continuous-stream I2S tone synth)
es8311.{c,h},        vendored Espressif/Waveshare ES8311 driver (Arduino I²C HAL)
  es8311_reg.h
ui.{cpp,h}          LVGL UI (~2400 lines) — all screens, overlays, result rows
board_config.h      ALL board pins (display, touch, PMIC, RTC, audio, buttons)
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

## 3. Hardware facts (verified this session — trust these)

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
- **RTC:** PCF85063 at 0x51 (read-only; honors oscillator-stop flag → "Not set").
- **Buttons:** **BOOT = GPIO9** (active-low strapping pin). **PWR = PMIC key**.
- Unused hardware still on the board: QMI8658 IMU, SD/TF slot, Wi-Fi 6 / 802.15.4
  radios, RTC backup-battery pads. See `FEATURE_IDEAS.md`.

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

## 5. What happened this session (2026-07-22)

Large session. Highlights, newest first:

1. **Audio** added (ES8311). Two follow-up fixes: task priority raise (didn't
   fix it), then the real fix — **continuous I²S streaming** instead of
   stop/restart per tone (short tones were "cutting in and out"; a single
   continuous tone was clean → the restart was the glitch).
2. **UI perf:** QSPI 40→80 MHz, single 1/4 draw buffer (half the flush chunks,
   same RAM), refr period 30→16 ms. ~15–20 % faster redraws. Profiling showed
   the flush is partly **CPU-bound in Arduino_GFX** and rendering is ~half the
   cost — the real next lever is an async `esp_lcd` DMA flush (see §7).
3. **R-Test accuracy:** bidirectional sweep (drift cancellation), slope standard
   error `R ± σ` with tolerance-based "reliable", adaptive collect window.
4. **R-Test chart:** switched SCATTER→LINE (scatter renderer drew stray green
   edge line) and resample levels across full width.
5. **No-demo transition:** merged `origin` (which had removed the on-device demo
   + added simulator-app QA), then removed `El15Simulator`/`g_sim`/demo routing
   from our tree. Bench-testing is now the Android simulator over BLE.
6. Earlier in the session (commit `08f87b8`): touch-snap engine + responsiveness,
   header/layout redesign, Settings screen, R-test overhaul + estimator, the
   whole battery capacity test, adjustable sample rate, the two buttons, and the
   engine mutual-exclusion + manual-control-gating safety fixes.
7. A multi-agent QA audit ran; **2 confirmed high-severity findings were fixed**
   (both engines could drive the load at once; manual controls live during a
   test). The audit's full output had more findings — not all were triaged (see
   §7).

---

## 6. Verified good / not yet verified

**Verified on hardware:** clean boot (no panic/bootloop), ES8311 init, BLE
connect to a random-address peer, buttons don't phantom-fire at boot, audio tones
clean after the streaming fix, R-test V-I chart renders full-width with no stray
line, battery graph time axis smooth. Build is clean under `-Wall -Wextra`.

**Needs a human's eyes/ears (couldn't verify remotely):**
- Display integrity at **80 MHz** QSPI — user hasn't reported artifacts, but
  confirm no garbling/wrong colors. Fallback: 64 MHz or back to 40 in
  `display.cpp` `g_gfx->begin(...)`.
- Audio **latency** (inherent DMA depth) and any faint idle hiss in the 2.5 s
  keep-alive window.
- Full R-test / capacity runs against the **phone simulator or a real EL15**
  (no on-device demo now, so these can't be auto-driven from firmware).
- **Real ALIENTEK EL15** still untested in-house.

---

## 7. Open items & next steps (prioritized)

**Safety / correctness**
- **Triage the rest of the QA audit** (task `wyoedvuk0` output; only the top 2
  were fixed). Re-run `/code-review` or the workflow if needed.
- **BLE-drop supervisor during a capacity discharge:** on link loss the engine
  aborts, but a real pack keeps being sunk until the EL15's own UVP — document /
  recommend setting the hardware UVP; consider reconnect-and-stop (CAPACITY_PLAN
  §3).

**Biggest felt improvements**
- **Async `esp_lcd` DMA flush** — the real fix for UI lag. Arduino exposes
  `esp_lcd`, so it can be done in-place without leaving the Arduino build:
  replace the synchronous `draw16bitRGBBitmap` flush with an async panel + a
  done-callback calling `lv_disp_flush_ready`, restore double buffering. Roughly
  halves redraw cost. (User asked about Arduino vs IDF — conclusion: stay
  Arduino, drop to IDF APIs only here and for I²S if needed.)
- **SD card save** (Phase 0 of CAPACITY_PLAN): the "Save to SD" button is a stub
  for BOTH R-test and capacity. C6 has **no SDMMC** → must use SPI mode
  (SCK=11 MOSI=10 MISO=18; **CS pin unverified**). Unblocks logging/history/export.
- **NVS persistence:** brightness, volume/mute, sample rate, last battery config
  all reset on reboot. Highest quality-of-life-per-hour.
- **RTC set-time UI** (or NTP over the unused Wi-Fi) so logs get real timestamps.

**R-Test accuracy tier 2** (RTEST_ACCURACY.md §5, items 4–6, not done)
- Tare/zero step (subtract lead+contact resistance) — biggest low-R absolute-
  accuracy win. Curvature/residual flag. Average the priming Voc.

**Feature backlog:** see `FEATURE_IDEAS.md` (audio §14 largely done; buttons §15
partly done — hardware e-stop + sleep done, start/stop/screenshot not).

---

## 8. Gotchas & lessons (don't relearn these)

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

---

## 9. Companion docs

- `README.md` — build/flash + overview (also documents the ESP-IDF build).
- `QA_GUIDE.md` — advanced-QA test matrix & procedures (predates newest features;
  extend it as features land).
- `QA_REPORT.md` — earlier defect report (some items since fixed).
- `RTEST_ACCURACY.md` — R-test measurement methodology & remaining improvements.
- `CAPACITY_PLAN.md` — battery-test roadmap (SD phase still open).
- `FEATURE_IDEAS.md` — full feature/UX/audio/buttons backlog.
- `UI_DESIGN_BRIEF.md` — v2 "Focus" UI spec.

*Everything is committed and pushed. `backup/session-work-pre-merge` is a local
safety branch at the pre-merge snapshot (`08f87b8`) — safe to delete.*
