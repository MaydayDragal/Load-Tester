# EL15 Load Control — standalone ESP32 firmware

Turns a Waveshare touchscreen board into a self-contained controller for the
ALIENTEK **EL15** electronic load — no phone required. The board is the BLE
central, renders the instrument UI on its touch panel, and runs its own
resistance-sweep and battery-capacity test engines.

**Two board targets**, one PlatformIO env each (`src/board_config.h` selects the
pin map):

| Env | Board | Panel | State |
|---|---|---|---|
| `esp32-c6-amoled` | ESP32-C6-Touch-AMOLED-1.8 | 368×448 QSPI AMOLED | **hardware-verified** — every bench result in the docs is from this board |
| `esp32-s3-lcd35` | ESP32-S3-Touch-LCD-3.5 | 320×480 ST7796 SPI IPS | compiles; **never run on hardware** — see [`S3_BRINGUP.md`](S3_BRINGUP.md) |

The S3 board brings 8 MB of PSRAM and a hardware SDMMC host, which lifts the two
constraints that shape the C6 build (a 1/7-frame draw buffer fighting BLE for
contiguous heap, and a bit-banged SD link that makes a verified save take ~20 s).
Waveshare's "-C" SKU of that board is the same hardware bundled with an OV5640
camera, which this firmware does not use.

> **Status (2026-08-05): doing real work on real hardware.** Verified against a
> **real ALIENTEK EL15**: connect, telemetry, every mode opcode, setpoint,
> LOAD ON/OFF, a full continuous R-test sweep, and — the one that settles it —
> an **8.9 h unattended capacity discharge** of a 92 Ah lead-acid at 10 A,
> returning 88.58 Ah / 928.7 Wh / SoH 96.3 % and stopping itself at the cutoff.
>
> Two things are **not** trustworthy yet. The **SD card** has corrupted two of
> two real reports (a card-level fault; the save verification that should have
> caught it had four holes, since closed — [`HANDOVER.md`](HANDOVER.md) §17), so
> a save against a known-good card is still unproven. And **pause/resume and the
> link guard** have never been exercised against a genuine unattended drop.
> [`HANDOVER.md`](HANDOVER.md) §0 has the exact verified/unverified split;
> [`FIRST_CONTACT.md`](FIRST_CONTACT.md) has the bench procedure.

---

## Feature set

| Area | What it does |
|---|---|
| **Connect** | Scan (named devices only, deduped, random-address peers OK), connect, disconnect; optional auto-connect to the last device at boot |
| **Monitor** | V / I heroes (current turns red + "SINKING" under load), telemetry row (W · fan% · temp · runtime · Ah/mΩ), mode\|setpoint bar, pinned LOAD / RUN-TEST bar |
| **Adjust** | Dial-stepper with unit-aware step chips and hold-to-repeat, plus a numeric keypad |
| **Graph** | Live two-series auto-scaling V/I chart |
| **Modes** | CC / CV / CR / CP / CAP / DCR, plus **RT** and **BATT** UI-only pseudo-modes |
| **R-Test** | Fuse-aware **continuous current sweep** — a smooth triangular ramp (start → max → start) over an editable duration, fitting every reading. Live V / I / resistance graphs while it runs; result gives series resistance with a real **± uncertainty**, Voc, R², est. short-circuit current, sag, peak power, temp range, max fan; 2-wire/4-wire (Kelvin) with a shorted-probe **tare**; circuit-resistance estimator (wire mm²/length, connections, fuse type). See [`RTEST_ACCURACY.md`](RTEST_ACCURACY.md) |
| **Battery capacity** | Ten chemistry presets (Li-ion · LiPo HV · LiFePO4 · LTO · Na-ion · lead-acid 12 V · NiMH · NiCd · alkaline · custom) each with its per-cell voltage range and a cell-count ceiling that keeps a full pack under 60 V, cell count with Voc auto-suggest, auto cutoff, **C-rate chips that set the test current from the pack's rated capacity**, CC discharge with local Ah/Wh integration, debounced cutoff + safety caps, rest/rebound, live discharge curve, and a **time-to-cutoff estimate read off the chemistry's discharge curve** (pack internal resistance measured from the switch-on sag, capacity learned during the run — no nameplate rating required). See [`CAPACITY_PLAN.md`](CAPACITY_PLAN.md) |
| **Reports** | Real `RTEST_NNN.CSV` / `BATT_NNN.CSV` written to microSD, with an RTC timestamp when the clock has been set |
| **Settings** | Sample rate · **probe wiring (2-wire / 4-wire Kelvin + lead resistance, applied to every mode)** · connection/auto-connect · brightness · volume + mute · screen protection · clock (Wi-Fi NTP) · SD card check · battery · system info · restart |
| **Probe wiring** | Global 2-wire / 4-wire (Kelvin) setting. The load senses voltage at its own terminals, so a 2-wire hook-up reads short by the drop across your leads; give it the lead resistance (type it, or let R-Test measure it) and every reading is corrected back to the part — monitor, graph, battery cutoff and reports alike. 4-wire applies no correction. The voltage caption says which is in force |
| **Safety** | BOOT-button hardware e-stop · link-loss auto-stop supervisor · crash/reboot recovery · controller-brownout auto-off · load-safe power-off · engine mutual exclusion |
| **Audio** | ES8311 tones: tap click, button confirm, rising chime on completion, falling on error, urgent alarm on fault/e-stop |
| **Screen care** | AMOLED pixel shift + idle dim → true-black blank (suppressed while a test runs) |

There is deliberately **no on-device simulator**: simulation was done with an
Android **EL15 Load Simulator** app impersonating the load — including a full
battery discharge curve — over a *real* BLE link, so the firmware always
exercised its actual radio/transport path, never an in-process fake. That app
(and the Android control app it shipped beside) was **removed from the repo on
2026-08-03**; both live in git history, last present at commit `1cd5607`.
Check the simulator out from there for hardware-free bench testing.

**Not ported from the (since-removed) Android app:** the runtime/step/OCP bench tests,
on-device history browsing, PDF export, alarms, and the calibration sweep. The
engine + screen architecture is set up so these drop in as new engines and tabs.

---

## Architecture

```
main.cpp            owns objects, routes events, buttons, emergency stop
el15_protocol.h     wire protocol (header-only, pure): parse + command frames
el15_client.{h,cpp} BLE central (NimBLE 2.5): scan/connect/subscribe/poll/reassemble
el15_controller.h   El15Controller interface (the engines talk to this, not to BLE)
resistance_test.h   fuse-aware continuous sweep engine — triangular current ramp,
                    incremental (live) least-squares fit, uncertainty, tare
capacity_test.h     battery discharge / capacity engine (+ pack IR & charge-state ETA)
battery_model.h     chemistry OCV curves + standard test C-rates (header-only, pure)
display.{h,cpp}     CO5300/SH8601 AMOLED (QSPI 80 MHz) + touch + LVGL + touch-snap
                    + PMIC/RTC read+set + buttons + sleep + burn-in shift/dim
audio.{h,cpp}       ES8311 codec feedback (continuous-stream I2S tone synth)
es8311.{c,h}        vendored Espressif/Waveshare ES8311 driver (Arduino I²C HAL)
sd_card.{h,cpp}     microSD on bit-banged software SPI (SdFat) — own driver
report.h            CSV test reports (RTEST_/BATT_) written via sd_card
prefs.{h,cpp}       NVS persistence (debounced) + synchronous in-flight/creds flags
link_guard.h        link-loss auto-stop supervisor + crash recovery (header-only)
netclock.{h,cpp}    Wi-Fi scan + NTP → PCF85063 (radio powered only per op)
ui.{h,cpp}          LVGL UI — all screens, overlays, result rows
board_config.h      ALL board pins (display, touch, PMIC, RTC, audio, buttons, SD)
include/lv_conf.h   LVGL config (fonts, chart, refr period 16 ms, indev 10 ms)
```

**Threading contract (do not break this):** NimBLE host-task callbacks only
*enqueue* onto a FreeRTOS queue; `El15Client::loopTick()` drains it on the loop
task, so LVGL and both test engines are only ever touched single-threaded.
`main.cpp handleStatus()` fans a decoded status packet to `ui::onStatus()` plus
whichever engine is running. **Never call LVGL or an engine from a NimBLE
callback.** Audio runs on its own FreeRTOS task (priority 8) streaming I²S.

---

## Build & flash → PlatformIO

PlatformIO Core is typically installed **off-PATH**; the board's COM port can
hop between resets, so discover it rather than hard-coding it.

```bash
PIO=~/.platformio/penv/Scripts/pio.exe
PORT=$("$PIO" device list | grep -oE 'COM[0-9]+' | head -1)
E=esp32-c6-amoled          # or esp32-s3-lcd35
"$PIO" run -d firmware -e $E                                  # build (-Wall -Wextra on)
"$PIO" run -d firmware -e $E -t upload --upload-port "$PORT"  # flash
"$PIO" device monitor -p "$PORT" -b 115200                    # serial log
```

Omitting `-e` builds **both** targets, which is the cheap way to confirm a change
has not broken the other board.

Current builds, of the 3 MB `huge_app` slot: C6 **2.19 MB** (RAM 19.8 % static),
S3 **2.02 MB** (the S3 image is smaller mainly because it drops the software-SPI
SD driver for the SoC's SDMMC host). First build of each target downloads ~1 GB
(platform + toolchain + arduino-esp32 + libs) and the two targets need
*different* toolchains — RISC-V for the C6, Xtensa for the S3 — so the first S3
build after a C6-only checkout has a long download. Later builds are
incremental. Changing `include/lv_conf.h` forces a full LVGL recompile.

The ESP32-C6 needs **arduino-esp32 3.x** (IDF 5.1+), which mainline PlatformIO
doesn't ship — `platformio.ini` therefore uses the community
[pioarduino](https://github.com/pioarduino/platform-espressif32) platform fork.
Libraries are pinned **exactly** in `lib_deps` to the versions the
hardware-verified image was bench-tested against (they used to be `^` ranges,
which let a clean checkout resolve libraries no verified image ever ran):

| Pinned | Used for |
|---|---|
| `lvgl/lvgl @ 8.4.0` | UI toolkit (config in `include/lv_conf.h`) |
| `moononournation/GFX Library for Arduino @ 1.6.7` | SH8601 AMOLED driver |
| `h2zero/NimBLE-Arduino @ 2.5.0` | BLE central (2.x required — 1.x callback signatures differ) |
| `greiman/SdFat @ 2.3.1` | microSD over the custom software-SPI driver |

Notable build flags (all with reasons in `platformio.ini`):
`ARDUINO_LOOP_STACK_SIZE=12288`, `SPI_DRIVER_SELECT=3` (our own SdSpi driver),
`USE_SD_CRC=1`, `huge_app.csv` partitions, `qio_qspi` memory type.

### Test scaffolding (compiled out of normal builds)

| Flag | What it does |
|---|---|
| `EL15_SDTEST` | Boot-time SD info / write ×2 / readback, via the same paths the UI uses |
| `EL15_POLLTEST` | Poll-rate sweep, reporting fresh-vs-repeated frames per interval |
| `EL15_SELFTEST` | Safe mode-cycle sweep (load stays OFF) |
| `EL15_NO_POLL` | Disable polling entirely — proves whether telemetry is poll-driven |

```bash
PLATFORMIO_BUILD_FLAGS="-D EL15_SDTEST" "$PIO" run -d firmware -t upload --upload-port "$PORT"
```

### Build & flash → ESP-IDF (unverified)

An ESP-IDF project (`CMakeLists.txt`, `main/`, `sdkconfig.defaults`,
`partitions.csv`) also exists in the tree, compiling the same `src/` with
arduino-esp32 as a component. As of 2026-08-03 its file list covers all ten
translation units, its partition table carries the `spiffs` partition the
LittleFS datapoint log needs, and its LVGL pin matches the 8.4 the code is
adapted to — but **no ESP-IDF build has actually been run**; only the file
lists and versions have been reconciled. The PlatformIO/Arduino build is the
one that is built, flashed, and hardware-verified. Vendored-library setup:
`components/README.md`.

### Arduino IDE alternative

Install **esp32 by Espressif 3.0.0+** (Boards Manager), select an ESP32-C6
board, add the four libraries above via Library Manager, copy
`include/lv_conf.h` next to your LVGL library folder (or keep
`LV_CONF_INCLUDE_SIMPLE` on the include path), then open `src/*` as a sketch.

---

## Hardware notes (verified on this board)

- **No PSRAM.** 512 KB on-chip HP SRAM (~320 KB usable), 16 MB flash. LVGL uses
  a **1/7-frame (64-line, ~47 KB)** partial draw buffer — sized so NimBLE keeps
  the ≥~30 KB contiguous block it needs to *establish* a connection. Growing the
  buffer breaks BLE connects (HCI 0x3e); see `BUF_LINES` in `display.cpp`.
- **Display:** Waveshare call it **CO5300**; driven with Arduino_GFX's
  `Arduino_SH8601` (compatible command set) over QSPI at **80 MHz**.
  Reset/panel-enable via the **TCA9554 expander (0x20) bits 4,5**.
  `LV_COLOR_16_SWAP 0`.
- **Touch:** Waveshare call it **CST820**; this unit answers on the **FocalTech**
  (FT3168/FT6x36) register map. `touchInit()` writes both families' anti-sleep
  registers so idle auto-sleep is defeated either way. I²C SDA=8 SCL=7 addr 0x38.
- **Audio:** **ES8311** at I²C 0x18 (shared bus). I²S MCLK=19 BCLK=20 DIN=21
  WS=22 DOUT=23; speaker-amp power-enable is **TCA9554 bit 7**.
- **PMIC:** AXP2101 at 0x34 (battery %, VBAT, USB present, power-off). The
  **PWR key** arrives as PMIC IRQ bits, not a GPIO.
- **RTC:** PCF85063 at 0x51, readable *and* settable (NTP sync).
- **SD:** SPI mode (the C6 has no SDMMC host) on **bit-banged software SPI**,
  SCK=11 MOSI=10 MISO=18 CS=6 — deliberately independent of the panel's SPI2.
- **Buttons:** BOOT = GPIO9 (active-low strapping pin) · PWR = PMIC key.
- Unused so far: QMI8658 IMU, 802.15.4 radio, RTC backup-battery pads.

---

## Using it

1. Power the board over USB; the instrument UI comes up.
2. **Connect** → Scan → pick your EL15. (Settings ▸ Connection can make this
   automatic on the next boot.)
3. **Monitor** shows the live readout; the **mode\|set** bar and **Adjust**
   drive the load; the pinned bar toggles LOAD.
4. **R-Test** runs the fuse-aware sweep; **Battery** runs a capacity discharge.
   Both offer **Save to SD card** on their result screen.
5. **BOOT** is a hardware emergency stop from any screen. **PWR** short-press
   sleeps/wakes the screen; a long press powers the controller off *after*
   forcing the load off.

To test with no load hardware, run the Android **EL15 Load Simulator** app on a
second phone: it advertises as an EL15 over real BLE and models either a fixed
circuit or a full battery with a chemistry-accurate discharge curve.

> ⚠ **Safety** — this drives real current. Set the EL15's own hardware UVP as a
> backstop before discharging a pack (the firmware's link guard needs a working
> radio to act; the instrument's UVP does not), keep the controller on USB for
> long unattended runs, and stay clear of the setup while a test runs.

---

## Companion docs

| Doc | What's in it |
|---|---|
| [`HANDOVER.md`](HANDOVER.md) | **Read first.** Living session handover: current state, hardware facts, gotchas, open items |
| [`FIRST_CONTACT.md`](FIRST_CONTACT.md) | Ordered bench checklist for working with the real EL15 |
| [`QA_GUIDE.md`](QA_GUIDE.md) | Per-feature test matrix and procedures |
| [`QA_REPORT.md`](QA_REPORT.md) | 2026-07-21 code audit + its resolution status |
| [`RTEST_ACCURACY.md`](RTEST_ACCURACY.md) | R-test measurement methodology and remaining improvements |
| [`CAPACITY_PLAN.md`](CAPACITY_PLAN.md) | Battery-test roadmap and phase status |
| [`FEATURE_IDEAS.md`](FEATURE_IDEAS.md) | Full feature / UX / audio / buttons backlog |
| [`UI_DESIGN_BRIEF.md`](UI_DESIGN_BRIEF.md) | v2 "Focus" UI spec (design-time brief) |
