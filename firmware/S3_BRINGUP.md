# ESP32-S3-Touch-LCD-3.5 — bring-up checklist

The port to the Waveshare **ESP32-S3-Touch-LCD-3.5** compiles and is complete in
the sense that every subsystem has a code path for this board. **Nothing below
has ever run on the hardware** — the board was not in hand when the port was
written, and every pin came from reading Waveshare's demo sources, not a
schematic and not a running device.

So treat this as a list of *predictions to falsify*, in an order chosen so each
step depends only on the ones before it. Work down it with the board on the
bench; anything that fails tells you which assumption in `src/board_s3_lcd35.h`
was wrong, and the "if it fails" column says where to look first.

**Related docs:** [`HANDOVER.md`](HANDOVER.md) §0 for where the project stands,
[`QA_GUIDE.md`](QA_GUIDE.md) for the per-feature test matrix (written against the
C6 board — the *behaviour* it describes should be identical here),
[`FIRST_CONTACT.md`](FIRST_CONTACT.md) for the safe order of approaching a real
EL15, which is unchanged by the board swap.

---

## 0. Before you plug anything in

```bash
PIO=~/.platformio/penv/Scripts/pio.exe
"$PIO" run -d firmware -e esp32-s3-lcd35                       # build
"$PIO" device list                                             # find the port
"$PIO" run -d firmware -e esp32-s3-lcd35 -t upload --upload-port COMx
"$PIO" device monitor -p COMx -b 115200
```

The C6 board is still a first-class target — `-e esp32-c6-amoled` builds the
hardware-verified image, and `pio run` with no `-e` builds both. Nothing in this
port changed C6 behaviour (its image is the same size to within a few bytes).

**Do not attach the EL15 or any load for steps 1–8.** None of them need it, and
a board whose pin map is still unproven has no business commanding a load.

---

## 1. It boots at all

| Check | Expect | If it fails |
|---|---|---|
| Serial comes up at 115200 | `[boot] reset reason:` line | USB CDC: the env sets `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1`; if the port enumerates but is silent, try a hard reset while the monitor is attached |
| No bootloop | one boot banner, then the UI | a panic loop here is almost always PSRAM (step 2) |

## 2. PSRAM came up — **check this before anything visual**

This is the single assumption most likely to be wrong, and it silently degrades
everything else, so confirm it early.

| Check | Expect | If it fails |
|---|---|---|
| Draw buffers allocated in PSRAM | **absence** of `[display] PSRAM draw-buffer alloc FAILED` | that message means `board_build.arduino.memory_type` is wrong. It is `qio_opi` (quad flash + octal PSRAM, the N16R8 layout). Try `board_build.flash_mode = dio` in the `esp32-s3-lcd35` env — Waveshare's own ESP-IDF build ends up flashing DIO because octal PSRAM crowds the QIO flash pins |
| `ESP.getPsramSize()` if you add a print | ~8 MB | if 0, as above |

The firmware deliberately **falls back** to a smaller internal-RAM buffer rather
than refusing to boot, so a UI that works but feels sluggish, with that message
in the log, means PSRAM is absent — not that the panel is slow.

## 3. Panel

| Check | Expect | If it fails |
|---|---|---|
| Backlight lights | screen visibly lit after boot | `LCD_BL_GPIO` (6) or the LEDC attach. A dark-but-working panel (you can see content at an angle) is a backlight problem, not a panel problem |
| Something renders | the v2 UI, not noise | `LCD_SPI_MOSI/SCK/DC` (1/5/3), and the TCA9554 **bit 1** reset pulse — this board pulses reset LOW then releases, unlike the C6 which holds two bits high |
| Colours are right | dark UI, steel-blue accent `#7ba1c9`, **not** inverted, **not** red/blue swapped | inversion → the `ips = true` argument to `Arduino_ST7796`; red/blue swapped → `LV_COLOR_16_SWAP` in `include/lv_conf.h` (currently 0, paired with `draw16bitRGBBitmap`) |
| Geometry | 320×480 portrait, nothing clipped at the right edge | `LCD_WIDTH/HEIGHT`; the menu tiles size themselves from `UI_MENU_TILE_W` so they should fit automatically |
| Speed | screen transitions feel immediate | try raising `LCD_SPI_HZ` from 40 MHz to 80 MHz (Waveshare's ESP-IDF port uses 80). Corruption rather than a clean failure is the symptom of going too fast |

## 4. Touch

| Check | Expect | If it fails |
|---|---|---|
| Taps register | buttons respond, touch-snap feels like the C6 | `TOUCH_I2C_ADDR` 0x38. The FT6336 is the same FocalTech family as the C6's part, so `touchInit()` should need no change |
| Coordinates map correctly | tap lands where you touched, not mirrored or rotated | the vendor's LVGL config uses `mirror_x = 1` at rotation 0. **This is the most likely touch defect.** If X is mirrored, invert it in `readTouch()` (`x = LCD_WIDTH - 1 - x`) |
| No dead first-tap after idle | first tap after 30 s idle registers | the auto-sleep disable writes in `touchInit()` |

## 5. Buttons, PMIC, RTC

| Check | Expect | If it fails |
|---|---|---|
| BOOT button | short press acts as e-stop; long press per `QA_GUIDE` | `BOOT_BTN_GPIO` is **0** on this board. If it reads as permanently pressed, you may have inherited the C6's GPIO 9 — which here is the SD card's DAT0 |
| PWR key | short/long press events arrive | AXP2101 IRQ bits at 0x34; the key is on the PMIC, not a GPIO, on both boards |
| Battery readout | plausible % and mV in Settings | AXP2101 register map is shared with the C6 and should need nothing |
| RTC | set the clock, reboot, time survives | PCF85063 at 0x51, same as the C6 |

## 6. Audio

| Check | Expect | If it fails |
|---|---|---|
| `[audio] ready (ES8311)` | present in the boot log | `ES8311_I2C_ADDR` 0x18 |
| Tap click audible | clean tone, no glitching | I2S pins 12/13/15/16. **If tones are silent but init succeeded, suspect DOUT/DIN are swapped** — Waveshare's own comments on those two pins contradict their code, and this port follows the code (16 = to codec/speaker) |
| No amp-enable needed | sound works with no expander write | this board hardwires the amp on; `ampEnable()` is a deliberate no-op here |

## 7. SD card — **the biggest behavioural change**

The C6 bit-bangs SPI at a few hundred kHz. This board uses the hardware SDMMC
host in 1-bit mode, which should be roughly a hundred times faster.

| Check | Expect | If it fails |
|---|---|---|
| Settings ▸ SD card check | `SDHC nn.n GB` | `SD_MMC_CLK/CMD/D0` (11/10/9). Only those three lines are wired — 1-bit mode is mandatory, and SPI-mode SD is impossible here (no D3) |
| Save a report | writes **and verifies** in well under a second | the verified-save path (CRC-32 read-back) is unchanged; only the transport differs |
| `[sd] verify OK: … read back and matched` | present on every save | if verification fails on a good card, suspect the `fsSync`/`fsClose` shim — the VFS reports write errors through the Print error flag rather than a return code |
| FAT timestamp | file's modified date in a PC listing matches the RTC | this board has **no SdFat callback**; it pushes the RTC into the system clock at mount (`syncSystemClockFromRtc`). A 1970 date means the RTC was unset or that call did not run |
| Eject / re-insert | next save re-inits and works | `cardResponds()` uses `cardType()` here rather than a CID round-trip — a weaker check than the C6's, and worth a deliberate test |

## 8. BLE

| Check | Expect | If it fails |
|---|---|---|
| Scan finds devices | named peers listed | nothing board-specific; NimBLE config is shared |
| Connect + telemetry | ~17–19 fresh samples/s | the C6's contiguous-heap pressure that forced the tiny draw buffer **does not exist here** — if BLE connects fail with HCI 0x3e on this board, that is a new bug, not the old memory one |

## 9. Only now: the EL15

Everything above is bench-safe. From here follow
[`FIRST_CONTACT.md`](FIRST_CONTACT.md) unchanged — connect, telemetry, mode and
setpoint at zero load, then LOAD ON at a small current, then the R-test and
capacity engines. The engines themselves are board-independent code that has
been exercised on real hardware from the C6; what you are re-proving is that the
new board drives them identically.

---

## What this port did NOT change

Worth knowing so you do not go looking for board causes for these:

- **Every test engine** (`capacity_test.h`, `resistance_test.h`,
  `battery_model.h`, `report.h`) is untouched and board-independent.
- **The BLE client and protocol** are untouched.
- **The UI** changed in exactly one respect: the 2-column menu tiles derive
  their width from the panel instead of a hard-coded 164 px. Everything else was
  already flex-laid-out and resolution-agnostic.
- **Screen care**: pixel-shift burn-in mitigation now defaults **off** on this
  board (an IPS LCD does not burn in). Idle dim/blank still applies and matters
  more here, since the backlight is where the display power goes.
- **Low-memory mode** (the Wi-Fi RAM window) is a no-op on this board. With
  PSRAM there is nothing to free, and doing it anyway would have moved the draw
  buffer out of PSRAM into the internal RAM Wi-Fi wants.
