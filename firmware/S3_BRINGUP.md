# ESP32-S3-Touch-LCD-3.5B — bring-up

The board arrived on **2026-08-10** and is a Waveshare **ESP32-S3-Touch-LCD-3.5B-C**.

That matters more than the "B" suggests. This port was originally written for the
**ESP32-S3-Touch-LCD-3.5** (no B) from vendor sources with no hardware in hand.
The two are different products: the 3.5B drives its panel with an **AXS15231B
over QSPI**, the 3.5 with an **ST7796 over 4-wire SPI**. The first image flashed
to this board rendered multi-coloured static. The target has been repointed at
the 3.5B; the plain 3.5 is no longer supported, and `git log` has the old
`board_s3_lcd35.h` if one ever appears.

This document is now a record of what was actually falsified and what remains
open, rather than the list of predictions it started as.

**Related docs:** [`S3_UI_HANDOVER.md`](S3_UI_HANDOVER.md) for the UI designer's
view of this panel, [`HANDOVER.md`](HANDOVER.md) §0 for where the project stands,
[`QA_GUIDE.md`](QA_GUIDE.md) for the per-feature test matrix (written against the
C6 — the *behaviour* it describes should be identical here),
[`FIRST_CONTACT.md`](FIRST_CONTACT.md) for the safe order of approaching a real
EL15, which is unchanged by the board swap.

---

## Status

| # | Subsystem | State |
|---|---|---|
| 1 | Boots, no bootloop | ✅ verified |
| 2 | PSRAM | ✅ verified — 8 MB up |
| 3 | Panel | ✅ verified — renders the UI correctly |
| 4 | Touch — taps | ✅ verified — accurate |
| 4b | Touch — **scrolling** | ❌ **open** — swipes fire the item under the finger |
| 5 | PMIC / RTC / IMU / expander / codec | ✅ present on I²C; PMIC read verified |
| 5b | Buttons (BOOT, PWR key) | ⬜ not yet exercised |
| 6 | Audio | ❌ **open** — I²S init fails |
| 6b | Backlight brightness | ⚠️ dim at full duty; cause not established |
| 7 | SD card | ⬜ not yet exercised |
| 8 | BLE | ✅ verified — scan, connect, and streaming telemetry |
| 9 | The EL15 itself | ⬜ not yet |

---

## Build and flash

```bash
PIO=~/.platformio/penv/Scripts/pio.exe
"$PIO" run -d firmware -e esp32-s3-lcd35b                       # build
"$PIO" device list                                              # find the port
"$PIO" run -d firmware -e esp32-s3-lcd35b -t upload --upload-port COMx
"$PIO" device monitor -p COMx -b 115200
```

The C6 is still a first-class target — `-e esp32-c6-amoled` builds the
hardware-verified image, and `pio run` with no `-e` builds both. Nothing in this
port changes C6 behaviour.

The board enumerates as a native USB CDC port (VID:PID `303A:1001`), and
`esptool ... chip_id` reports `ESP32-S3 (QFN56) revision v0.2`, `Embedded PSRAM
8MB (AP_3v3)`, 16 MB flash, `eFuse: quad`.

> Waveshare's factory image was backed up before the first flash. If you need it,
> `esptool --port COMx write_flash 0 waveshare-s3-lcd35b-factory-flash.bin`.

---

## What the boot log tells you

A healthy boot on this board looks like this — about 18 lines, not thousands:

```
[prefs] loaded (inFlight=0, ssid=-)
[i2c] scan: 0x18 0x20 0x34 0x3B 0x51 0x6B  (6 devices)
[boot] reset reason: ...
[boot] heap after UI: 57040 B free, largest block 31732 B (...)
[boot] psram: 8388608 B total, 7769852 B free, largest block 7733236 B
[pmic] controller battery: ABSENT (running on USB), ...
```

**The I²C scan is the single most useful line.** Every peripheral except the
panel hangs off that one bus, so a missing address explains every later failure
that touches it:

| Address | Part |
|---|---|
| 0x18 | ES8311 audio codec |
| 0x20 | TCA9554 I/O expander (panel reset is bit 1) |
| 0x34 | AXP2101 PMIC (battery, PWR key) |
| **0x3B** | **AXS15231B touch** — *not* 0x38 |
| 0x51 | PCF85063 RTC |
| 0x6B | QMI8658 IMU (unused) |

**`[boot] psram: ... 0 B total` is the unambiguous PSRAM failure.** It has never
happened on this board — `board_build.arduino.memory_type = qio_opi` is correct
and the `flash_mode = dio` fallback this document used to recommend was never
needed.

---

## What was wrong, and how it was found

Worth reading before debugging anything else here: four of these were silent, and
none of them failed at build time.

**The panel transport.** Multi-coloured static. The board is a 3.5B, so the
ST7796-over-SPI path could never have worked. Arduino_GFX 1.6.7 already ships
`Arduino_AXS15231B` and `Arduino_ESP32QSPI`, so no dependency bump was needed —
the version pinning `QA_GUIDE` §1 ties to hardware-verified results is intact.

**Touch at the wrong address.** Nothing answers at 0x38. Polling it emitted ~90
failed I²C transactions per second — 2095 of them in a 25 s capture, burying
every other line in the log. The part is at 0x3B and is not a register file at
all: it answers a fixed 11-byte command with a 14-byte report.

**Four GPIOs assigned to other jobs**, none of which fails at build time:

| Pin | Actually | The old header said |
|---|---|---|
| 12 | panel QSPI CS | I²S MCLK |
| 4 | panel QSPI D3 | "the ONLY general-purpose pin not spoken for" |
| 2 | panel QSPI D1 | panel MISO |
| 3 | panel QSPI D2 | panel DC |

The GPIO 12 collision is the dangerous one: it only stayed harmless because
audio init was failing for an unrelated reason. I²S MCLK is **44** here.

**The panel will not accept a partial write.** A flush that does not start at
row 0 and span the full width walks out of step with the controller's line
stride, and every line after the first lands offset. Symptom: a recognisable
first paint with every other line shifted, then outright garbage when a tap
repainted one widget. Rounding the flush to full *width* alone changed nothing —
which is what ruled out a column-alignment quirk and pointed at the row start.
The fix is LVGL `full_refresh = 1` with full-frame buffers, which reproduces what
Waveshare's demos do: they drive the panel through an `Arduino_Canvas` and push
the whole framebuffer every time, so there is no partial write anywhere in their
code.

**The panel bus was over-clocked.** After the above, roughly every fourth row of
pixels was still displaced 2–3 px right. `LCD_SPI_HZ` had been set to 80 MHz by
analogy with the C6. Waveshare call `gfx->begin()` with no argument, taking
Arduino_GFX's `ESP32QSPI_FREQUENCY` default of **40 MHz** — half. At 40 MHz the
artifact is gone. This is exactly the failure mode predicted for this bus:
corruption rather than a clean error is what "too fast" looks like.

---

## Open: touch scrolling

Taps land accurately. **Swipes do not scroll** — a drag fires the item under the
finger the moment it lands.

The first attempt at this was to stop reporting malformed frames as releases. The
AXS15231B emits partial frames mid-gesture, and the vendor's `bsp_touch_read()`
returns early *without* modifying its stored touch data, so the last point and
the finger-down state both persist. This port was translating those rejections
into "no touch", which reads to LVGL as press-then-release — a click, and never
enough accumulated movement to become a scroll. `readTouch()` now routes
malformed frames to its `-1` ("transient, hold previous state") return instead,
with a 400 ms watchdog so a missed release frame cannot strand a press.

**That did not fix it.** So the next step is to stop reasoning about the frames
and read them: log the raw 14 bytes during a slow drag and see what the part
actually sends between touch-down and lift. Until that is done, treat the
paragraph above as a plausible but unconfirmed model of this controller.

Note the touch-target snapping in `display.cpp` shifts a whole gesture by the
offset computed at press time; it was tuned for the C6's ~320 DPI 1.8" panel and
is a second thing worth suspecting on a 3.5" one.

## Open: audio

I²S init fails:

```
gdma: gdma_register_tx_event_callbacks(): user context not in internal RAM
i2s_common: i2s_init_dma_intr(): Register tx callback failed
ESP_I2S.cpp initSTD(): ERROR: ESP_ERR_INVALID_ARG
```

This is **not our bug and not a pin problem** — it is an incompatibility inside
arduino-esp32's own prebuilt S3 config:

```
# CONFIG_I2S_ISR_IRAM_SAFE is not set   -> I2S allocates its channel object with
                                           MALLOC_CAP_DEFAULT, which on a PSRAM
                                           board can land in external RAM
CONFIG_GDMA_ISR_IRAM_SAFE=y             -> GDMA rejects a callback user context
                                           that is not in internal RAM
```

Those two are mutually exclusive on any PSRAM board using I²S. Pinning the
allocator with `heap_caps_malloc_extmem_enable(SIZE_MAX)` around the init was
tried and does **not** work, because the driver calls
`heap_caps_calloc(..., MALLOC_CAP_DEFAULT)` directly and that bypasses the
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` split which that knob controls. That
attempt was reverted rather than left in place looking like a fix.

The C6 has no PSRAM and so never hits this.

## Open: brightness

The panel reads dim. `ledcAttach()` succeeds (it is checked and logged now), and
the default is full duty, so the obvious explanations are already excluded. Note
`prefs::change()` writes *all* keys, so any device that has ever had a setting
changed already has the old 200 in NVS and keeps it until the slider is moved —
check the slider position before concluding the hardware is at fault.

---

## Still to do

**Buttons, PMIC, RTC (step 5b).** BOOT is GPIO **0** here, not the C6's GPIO 9 —
on this board GPIO 9 is the SD card's DAT0, so the old pin would have read the SD
bus as a button. The PWR key is on the PMIC, not a GPIO, on both boards. Set the
clock, reboot, confirm the time survives.

**SD card (step 7)** — the biggest behavioural change. The C6 bit-bangs SPI at a
few hundred kHz; this board uses the hardware SDMMC host in 1-bit mode, which
should be ~100× faster. Only CLK/CMD/D0 are wired (11/10/9), so 1-bit is
mandatory and SPI-mode SD is impossible (no D3). Check: Settings ▸ SD card check
reports a size; a report saves *and verifies* in well under a second; `[sd]
verify OK` appears on every save; the FAT timestamp matches the RTC (this board
has no SdFat callback and instead pushes the RTC into the system clock at mount —
a 1970 date means that did not run); eject/re-insert re-inits.

**BLE (step 8) — done.** Scan, connect and streaming telemetry all verified. The
status line in the log appears once a second, but that is a deliberate
rate-limited proof-of-life print; the actual poll rate is `pollMs = 50`, i.e.
20 Hz.

Getting there needed a real fix, and it is the one worth knowing about on any
PSRAM board. Connecting used to **panic and reboot**:

```
ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM)
  npl_os_freertos.c line 445, npl_freertos_callout_init
  expression: esp_timer_create(&create_args, &co->handle)
```

NimBLE creates an `esp_timer` while establishing a connection and wraps it in
`ESP_ERROR_CHECK`, so a failed allocation is an `abort()`, not a returned error.
Scanning never allocates one, which is why scanning looked fine.

The board had 8 MB of PSRAM free and 57 KB of internal heap, because **LVGL was
building the entire widget tree in internal SRAM**: it allocates every object and
style from the C heap, they are all small, and
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` in the prebuilt libs serves anything
under 4 KB from internal RAM even when PSRAM is empty. Pointing `LV_MEM_CUSTOM_*`
at a PSRAM-preferring allocator (`include/lv_mem_psram.h`) moved it:

| | before | after |
|---|---|---|
| internal free | 57 012 B | **255 896 B** |
| largest free block | 31 732 B | **188 404 B** |

So the old `[boot] heap after UI` warning about needing ~30 KB contiguous was
genuinely marginal on this board, not comfortably clear as the port assumed.
Internal SRAM is the scarce resource here and the only place the BLE stack, DMA
buffers and ISR-context allocations can live — keep large, non-DMA things out of
it.

**Only then, the EL15 (step 9).** Follow [`FIRST_CONTACT.md`](FIRST_CONTACT.md)
unchanged. The test engines are board-independent and have been exercised on real
hardware from the C6; what you are re-proving is that this board drives them
identically.

---

## What this port did NOT change

So you do not go looking for board causes for these:

- **Every test engine** (`capacity_test.h`, `resistance_test.h`,
  `battery_model.h`, `report.h`) is untouched and board-independent.
- **The BLE client and protocol** are untouched.
- **The UI** changed in one respect: the 2-column menu tiles derive their width
  from the panel instead of a hard-coded 164 px.
- **Screen care**: pixel-shift burn-in mitigation defaults **off** here (an IPS
  LCD does not burn in). Idle dim/blank still applies and matters more, since the
  backlight is where the display power goes.
- **Low-memory mode** (the Wi-Fi RAM window) is a no-op on this board. With PSRAM
  there is nothing to free, and doing it anyway would move the draw buffer out of
  PSRAM into the internal RAM Wi-Fi wants.
