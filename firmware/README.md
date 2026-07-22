# EL15 Load Control — standalone ESP32-C6 firmware

Turns a **Waveshare ESP32-C6-Touch-AMOLED-1.8** into a self-contained
controller for the ALIENTEK **EL15** electronic load — no phone required. The
board is the BLE central, renders the instrument UI on its AMOLED touch panel,
and runs the same reverse-engineered EL15 protocol and fuse-aware resistance
test as the Android app in this repo.

> **Status: first firmware pass (load-bearing core).** This was written against
> the named libraries and the board's published pin map, but it has **not been
> compiled or flashed on hardware** from this environment — there's no ESP
> toolchain or board here. Treat it as a working foundation to build, flash, and
> iterate on your device. The [hardware checklist](#before-you-flash-verify)
> below lists the handful of board-specific things to confirm first.

Builds **two ways from one source tree** (`src/`): with **PlatformIO** or with
**ESP-IDF** (`idf.py`). The ESP-IDF build uses arduino-esp32 as a component, so
the same `setup()`/`loop()` code compiles unchanged either way — pick whichever
toolchain you prefer.

## What's implemented in this pass

| Feature | Status |
|---|---|
| BLE central: scan, connect, subscribe FFF1, write FFF3, poll | ✅ |
| EL15 protocol (packet parse + command frames), byte-identical to the app | ✅ |
| Frame reassembly across notifications | ✅ |
| Live monitor: V / I / P / mode / temp / fan / load / protection | ✅ |
| Manual control: mode (CC/CV/CR/CP/CAP/DCR), setpoint, Load ON/OFF, Lock | ✅ |
| Fuse-aware circuit-resistance test with least-squares R + R² | ✅ |
| LVGL touch UI (top bar + Monitor / Control / R-Test tabs) | ✅ |

There is deliberately **no on-device simulator**: all simulation lives in the
Android **EL15 Load Simulator** app (`simulator/` in this repo), which
impersonates the load — including full battery discharge-curve simulation —
over a *real* BLE link. That way the firmware always exercises its actual
radio/transport path, never an in-process fake.

**Deferred to later passes** (the Android app has these; the firmware doesn't
yet): the four bench tests (capacity/runtime/step/OCP), on-device history &
graphs, CSV/PDF export, settings, alarms, and the calibration sweep. The
architecture below is set up so these drop in as new engines + tabs.

## Architecture (mirrors the Android app)

```
main.cpp            ← owns objects + routes events   (≈ DeviceCore)
├─ el15_protocol.h  ← wire protocol                   (= El15Protocol.kt)
├─ el15_client.*    ← BLE central transport           (= El15BleManager.kt)
├─ el15_controller.h← controller interface           (= El15Controller)
├─ resistance_test.h← fuse-aware sweep engine         (= CircuitResistanceTester.kt)
├─ display.*        ← SH8601 AMOLED + FT3168 + LVGL glue
├─ ui.*             ← LVGL screens
└─ board_config.h   ← ALL board-specific pins (verify these)
```

The protocol and test engines are line-for-line ports of the Kotlin, so the
firmware talks to the load identically to the phone.

## Build & flash → PlatformIO

```bash
# from the firmware/ directory
pio run                 # build
pio run -t upload       # flash over USB
pio device monitor      # serial log @ 115200
```

The ESP32-C6 needs **arduino-esp32 3.x** (IDF 5.1+), which mainline PlatformIO
doesn't ship yet — `platformio.ini` therefore uses the community
[pioarduino](https://github.com/pioarduino/platform-espressif32) platform fork.
Libraries are pinned in `lib_deps` and resolved automatically:

- `lvgl/lvgl @ ^8.3` — UI toolkit (config in `include/lv_conf.h`)
- `moononournation/GFX Library for Arduino @ ^1.4` — SH8601 AMOLED driver
- `h2zero/NimBLE-Arduino @ ^2.1` — BLE central

## Build & flash → ESP-IDF

The ESP-IDF project (`CMakeLists.txt`, `main/`, `sdkconfig.defaults`,
`partitions.csv`) compiles the same `src/` tree with arduino-esp32 as a
component. LVGL and arduino-esp32 are pulled by the component manager
(`main/idf_component.yml`); the two Arduino-only libraries are vendored once:

```bash
cd firmware
# one-time: drop the Arduino-only libs into components/ (see components/README.md)
git clone --depth 1 https://github.com/moononournation/Arduino_GFX.git /tmp/agfx
git clone --depth 1 https://github.com/h2zero/NimBLE-Arduino.git        /tmp/nimble
cp -r /tmp/agfx/src   components/Arduino_GFX/
cp -r /tmp/nimble/src components/NimBLE-Arduino/

idf.py set-target esp32c6
idf.py build flash monitor
```

`CONFIG_AUTOSTART_ARDUINO=y` (in `sdkconfig.defaults`) makes IDF run the
Arduino `setup()`/`loop()`, so no `app_main()` is needed. LVGL is configured
from `include/lv_conf.h` via `LV_CONF_INCLUDE_SIMPLE` (set in
`main/CMakeLists.txt`); alternatively configure it through
`idf.py menuconfig → Component config → LVGL`.

### Arduino IDE alternative

Install **esp32 by Espressif 3.0.0+** (Boards Manager), select an ESP32-C6
board, add the three libraries above via Library Manager, copy
`include/lv_conf.h` next to your LVGL library folder (or keep
`LV_CONF_INCLUDE_SIMPLE` on the include path), then open `src/*` as a sketch.
NimBLE-Arduino 2.x is required — the 1.x callback signatures differ.

## Before you flash: verify

1. **Panel pins** — `board_config.h` holds every board-specific GPIO (QSPI
   data/clock/CS, reset, touch I2C). They follow Waveshare's published
   ESP32-C6-Touch-AMOLED-1.8 example, but **cross-check them against the
   [board wiki](https://www.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-1.8)** and
   their Arduino demo's config header — revisions vary, and some control lines
   may sit behind the on-board TCA9554 I/O expander (`LCD_RST_VIA_EXPANDER`).
2. **Colour order** — if colours look inverted/swapped, flip `LV_COLOR_16_SWAP`
   in `include/lv_conf.h`.
3. **Touch chip** — the FT3168 driver in `display.cpp` uses the common
   FT6x36-family register map (0x02 count, 0x03.. coordinates). If touches don't
   register, confirm the address (`TOUCH_I2C_ADDR`) and that the touch panel
   isn't held in reset.
4. **Brightness** — `display::setBrightness` calls `Arduino_SH8601::setBrightness`;
   if your Arduino_GFX version lacks it, update the library.
5. **BLE address type** — `El15Client::connectTo` uses `BLE_ADDR_PUBLIC`. If a
   scan finds your EL15 but a direct reconnect fails, it may advertise a random
   address — switch to `BLE_ADDR_RANDOM`.

## Using it

1. Power the board over USB; the instrument UI comes up.
2. Tap **Connect** → pick your EL15 from the scan list.
3. **Monitor** tab shows the live readout. **Control** sets mode / setpoint /
   load. **R-Test** runs the fuse-aware resistance sweep and shows R, Voc, R².

To test with no load hardware, run the Android **EL15 Load Simulator** app on a
second phone: it advertises as an EL15 over real BLE and models either a fixed
circuit (a resistance test recovers its configured emf / series R) or a full
battery with a chemistry-accurate discharge curve. The ESP connects to it
exactly as it would to the real instrument.
