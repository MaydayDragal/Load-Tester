# EL15 Controller Firmware — Session Handover

Living handover for the **standalone ESP32-C6 firmware** (`firmware/`) that turns
a Waveshare ESP32-C6-Touch-AMOLED-1.8 into an on-device controller for the
ALIENTEK EL15 electronic load. Update this at the end of each session.

**Last updated:** 2026-07-24.
**Branch:** `claude/android-apk-load-tester-k82q4g` (= `main`). Committed + merged.

---

## 0. Latest session (2026-07-24) — FIRST REAL EL15 + SD card working

This was the first session against a **real ALIENTEK EL15** (all prior work was
simulator-only). Four things were found and fixed on hardware — read these before
touching the BLE or SD code:

1. **Command checksums (THE big one).** Load control did nothing on the real
   unit — telemetry worked but mode/setpoint/LOAD_ON were silently ignored.
   Cause: every command frame needs a trailing checksum byte that makes the whole
   frame sum to 0 (mod 256); the DM40GUI-derived constants only had it on POLL.
   Fixed in `el15_protocol.h` (`frameChecksum()` + baked-in bytes). Proven: with
   polling disabled ALL telemetry stopped, so POLL is the only thing that ever
   worked — and it's the only checksummed frame. See §10.
2. **Connect crash.** `getCharacteristics(true)` re-discovered the service and
   freed the notify/write characteristic objects we'd just resolved → use-after-
   free → reboot on connect. Now `getCharacteristics(false)`.
3. **Dropped no-response writes.** FFF3 is write-no-response only, and the device
   drops a command that lands too close behind another no-response write. Two
   symptoms — mode changes colliding with the 500 ms poll, and the R-test's
   `setSetpoint`→`setLoad(true)` back-to-back. Fix in `El15Client::writeRaw`:
   pace every CONTROL write ≥50 ms from the previous one AND defer the next poll.
4. **SD card writes now work** — on **bit-banged software SPI**, because the C6's
   one SPI host is owned by the panel and the IDF sdspi driver can't share it.
   See §10.

Also: **default poll rate is now 50 ms (20 Hz)** — a sweep showed the EL15 tops
out at ~17-19 fresh samples/s, so 20 Hz captures ~all of it; faster just refetches
repeats and floods the link (§11). Serial now also carries useful bring-up logs:
a per-connect **GATT characteristics dump**, a per-control-write line
(`[ble] write OK/FAIL …`), and a 1 Hz `[ble] status rx: …` line. They're low-noise
and handy on hardware; strip them (in `el15_client.cpp`) for a silent build.

**Test scaffolding** lives behind build flags, compiled out of normal builds:
`-D EL15_SDTEST` (boot-time SD read/write/readback in `main.cpp`),
`-D EL15_POLLTEST` (poll-rate sweep in `el15_client.cpp`),
`-D EL15_SELFTEST` (safe mode-cycle sweep). Flash e.g.
`PLATFORMIO_BUILD_FLAGS="-D EL15_SDTEST" "$PIO" run -d firmware -t upload …`.

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
Read serial passively with a `System.IO.Ports.SerialPort` one-liner (opening the
port asserts DTR/RTS and resets the board, which is usually what you want).
**If the board vanishes from USB entirely** (no COM port at all — happened once
under a very fast poll flood), physically unplug/replug the USB cable to recover.
Serial is 115200. Boot prints `[boot] reset reason: …` (power-on / PANIC / BROWNOUT / WDT / USB) —
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
resistance_test.h   fuse-aware sweep engine — bidirectional + slope uncertainty +
                    4-wire/tare correction (§6)
capacity_test.h     battery discharge / capacity engine
display.{h,cpp}     CO5300/SH8601 AMOLED (QSPI 80 MHz) + touch + LVGL + touch-snap
                    engine + PMIC/RTC read+set + buttons + sleep + burn-in shift/dim
audio.{h,cpp}       ES8311 codec feedback (continuous-stream I2S tone synth)
es8311.{c,h},        vendored Espressif/Waveshare ES8311 driver (Arduino I²C HAL)
  es8311_reg.h
sd_card.{h,cpp}     microSD on bit-banged software SPI (SdFat); own driver (§12)
report.h            CSV test reports (RTEST_/BATT_) written via sd_card
prefs.{h,cpp}       NVS persistence (debounced) + synchronous in-flight/creds flags
link_guard.h        link-loss auto-stop supervisor + crash-recovery (header-only)
netclock.{h,cpp}    Wi-Fi scan + NTP → PCF85063 (radio powered only per op)
ui.{cpp,h}          LVGL UI (~2700 lines) — all screens, overlays, result rows
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
- **Real ALIENTEK EL15**: load control, all modes, and telemetry now VERIFIED on
  hardware (§0, §10). Full R-test / capacity runs with real current still pending.
- **SD card**: read+write verified on hardware via the test scaffolding (§12);
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

## 7. Open items & next steps (prioritized)

**Safety / correctness**
- **Triage the rest of the QA audit** (task `wyoedvuk0` output; only the top 2
  were fixed). Re-run `/code-review` or the workflow if needed.
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
- **One SPI host, two peripherals:** the C6 has a single general-purpose SPI
  controller (SPI2) and the AMOLED owns it. The SD card is a second *device* on
  it, and `sd_card.cpp` swings the clock/data signals between the two pin sets
  through the GPIO matrix around each card access. Consequences to preserve:
  Arduino_GFX must be constructed with `is_shared_interface = true` (otherwise
  it holds the bus lock forever and the card hangs), the card is never left
  mounted, and nothing may draw while it is — the UI paints its "Writing to
  card..." state and calls `lv_refr_now()` *before* the blocking save.

---

## 9. Companion docs

- `README.md` — build/flash + overview (also documents the ESP-IDF build).
- `QA_GUIDE.md` — advanced-QA test matrix & procedures (predates newest features;
  extend it as features land).
- `QA_REPORT.md` — earlier defect report (some items since fixed).
- `RTEST_ACCURACY.md` — R-test measurement methodology & remaining improvements.
- `CAPACITY_PLAN.md` — battery-test roadmap (Phase 0 SD now landed).
- `FEATURE_IDEAS.md` — full feature/UX/audio/buttons backlog.
- `UI_DESIGN_BRIEF.md` — v2 "Focus" UI spec.

---

## 10. EL15 wire protocol on real hardware (2026-07-24)

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
- **Control writes must be paced** (see §0.3): ≥50 ms apart and clear of the poll,
  or the device drops the one that arrives too soon. `writeRaw` enforces this.
- **All 6 mode opcodes verified** on hardware (CC 01, CV 09, CR 11, CP 19,
  CAP 02, DCR 0A) — commanded mode == device-reported `b5`.

## 11. Poll-rate finding (2026-07-24)

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

## 12. SD card — bit-banged software SPI (2026-07-24)

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

**Still untested:** the actual UI Save path (Settings ▸ SD ▸ Check card, and the
R-test/battery Save buttons) — exercise once. Delete the `SDTEST_00N.CSV` files
left from testing.

---

*Everything is committed and merged to `main`.*
