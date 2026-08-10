// Board configuration for the Waveshare ESP32-S3-Touch-LCD-3.5B.
//
// Selected by BOARD_S3_LCD35B — see board_config.h. Do not include directly.
// Waveshare's "-C" SKU is this same board bundled with a camera module; the
// board itself is identical, and this firmware does not use the camera.
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ This board is NOT the ESP32-S3-Touch-LCD-3.5 (no B). The two are         │
// │ different products with different panel transports, and firmware for one │
// │ renders as multi-coloured static on the other. If the panel shows noise, │
// │ check which board is on the bench BEFORE touching any pin below.         │
// └─────────────────────────────────────────────────────────────────────────┘
//
//   Panel : 3.5" IPS LCD, 320 x 480, AXS15231B over QSPI (NOT 4-wire SPI, and
//           NOT an ST7796). Constructed with ips = false, which is what the
//           vendor demos pass and what renders non-inverted.
//   Touch : AXS15231B's own touch block at I2C 0x3B. This is a command/response
//           part, not a register-file part: it answers an 11-byte command with a
//           14-byte report. It shares the display controller — one chip does
//           both — so there is no separate touch IC to look for. INT and RST are
//           not wired, so it must be POLLED, which is what we do.
//   MCU   : ESP32-S3 (Xtensa dual-core, 240 MHz, Wi-Fi 4 / BLE 5)
//   RAM   : 512 KB on-chip SRAM PLUS 8 MB octal PSRAM. Confirmed on hardware:
//           `[boot] psram: 8388608 B total`. LVGL gets full double buffers in
//           PSRAM and NimBLE no longer competes with the UI for internal heap.
//   Also on board, unused by this firmware: QMI8658 6-axis IMU (0x6B) and the
//   camera interface on the -C SKU.
//
// PROVENANCE. Pin values come from Waveshare's own Arduino demos at
// github.com/waveshareteam/ESP32-S3-Touch-LCD-3.5B (examples 03/04/07/08/09 and
// libraries/esp_lcd_touch_axs15231b). The I2C bus and every address on it were
// then CONFIRMED on real hardware 2026-08-10 by the boot scan in display.cpp:
//
//   [i2c] scan: 0x18 0x20 0x34 0x3B 0x51 0x6B  (6 devices)
//
// Note the vendor's 08_gfx_helloworld says Wire.begin(21, 22). That is a
// copy-paste error from another board — the scan above is at 8/7 and finds all
// six devices, so 8/7 is correct.
#pragma once

// ---- Capability flags (what display.cpp / sd_card.cpp compile in) ----------
#define BOARD_NAME              "ESP32-S3-Touch-LCD-3.5B"
#define BOARD_PANEL_QSPI_AMOLED 0
#define BOARD_PANEL_QSPI_TFT    1   // Arduino_AXS15231B over Arduino_ESP32QSPI
#define BOARD_BACKLIGHT_PWM     1   // real backlight GPIO, driven by LEDC
#define BOARD_TOUCH_AXS15231B   1   // command/response touch, not FocalTech regs
#define BOARD_TOUCH_FOCALTECH   0
// Touch-target snapping OFF. It exists because the C6's 1.8" panel is ~322 DPI,
// where a 40 px control is about 3 mm. This panel is ~165 DPI (320x480 across
// 3.5"), so the same control is physically twice the size and presses do not
// need guessing at. Leaving it on actively broke the Settings screen: snapping
// pulled a press that landed on empty space onto the nearest switch or slider,
// which then consumed the drag, so the list could not be scrolled while every
// other screen could. Verified on hardware 2026-08-10.
#define BOARD_TOUCH_SNAP        0
#define BOARD_SD_SDMMC          1   // hardware SDMMC host, 1-bit
#define BOARD_SD_SOFT_SPI       0
#ifndef BOARD_HAS_PSRAM
#define BOARD_HAS_PSRAM         1   // 8 MB octal PSRAM (platformio.ini also sets this)
#endif

// ---- Panel geometry --------------------------------------------------------
#define LCD_WIDTH   320
#define LCD_HEIGHT  480

// ---- LCD QSPI bus (AXS15231B) ----------------------------------------------
// From 08_gfx_helloworld and 09_lvgl_arduino_v8, which agree exactly:
//     Arduino_ESP32QSPI(CS, CLK, D0, D1, D2, D3)
//     Arduino_AXS15231B(bus, RST, rotation, ips=false, 320, 480)
//
// Beware when reading the OLD plain-3.5 header: it assigned four of these pins
// to entirely different jobs — 1/2 as SPI MOSI/MISO, 3 as DC, 4 as "the only
// free GPIO", and 12 as I2S MCLK. On this board 12 is the panel's chip-select,
// so that last one would have had the audio clock driving panel CS.
#define LCD_QSPI_CS    12
#define LCD_QSPI_SCK   5
#define LCD_QSPI_D0    1
#define LCD_QSPI_D1    2
#define LCD_QSPI_D2    3
#define LCD_QSPI_D3    4
// RST is not a GPIO: the panel's reset hangs off the TCA9554 expander and is
// pulsed over I2C before the controller is initialised (see below).
#define LCD_RST_GPIO   -1
#define LCD_TE_GPIO    -1
// 40 MHz, which is Arduino_GFX's ESP32QSPI_FREQUENCY default and therefore the
// speed Waveshare's demos actually run at — they call gfx->begin() with no
// argument. Do NOT raise this to the C6's 80 MHz by analogy: that was tried on
// hardware 2026-08-10 and the panel rendered with roughly every fourth row of
// pixels displaced 2-3 px to the right. An over-clocked panel bus fails as
// progressive visual corruption rather than as a clean error, so a shift or
// tearing pattern like that means "too fast", not "wrong pins".
#define LCD_SPI_HZ     40000000

// The panel's reset line is TCA9554 bit 1, ACTIVE LOW. The vendor sequence is
// write high, then low, then high, then wait 200 ms — i.e. an ordinary
// active-low reset pulse, asserted momentarily and released. This is a different
// shape from the C6 board (which holds two bits HIGH to power a panel rail), so
// display.cpp keys off LCD_RST_EXPANDER_ACTIVE_LOW rather than reusing the C6's
// hold-high path.
#define LCD_RST_VIA_EXPANDER 1
#define LCD_RST_EXPANDER_ACTIVE_LOW 1
#define IO_EXPANDER_ADDR     0x20
#define LCD_EXPANDER_RST_BIT (1 << 1)
// Nothing else on this expander has a documented purpose, and the vendor demos
// touch only bit 1. Do not invent uses for the others without a schematic.

// ---- Touch (AXS15231B touch block, I2C) ------------------------------------
// Shared I2C bus (touch + TCA9554 + AXP2101 + ES8311 + PCF85063 + QMI8658, and
// the camera's SCCB on the -C SKU): SDA=8, SCL=7. Confirmed by the boot scan.
#define TOUCH_I2C_SDA  8
#define TOUCH_I2C_SCL  7
// 100 kHz, the Wire default and therefore what every Waveshare example for this
// board actually runs at — none of them call setClock(). The C6 runs this bus at
// 400 kHz and is verified there, but on this board the touch controller returns
// a large fraction of malformed frames: every byte identical, or the whole
// 14-byte frame shifted two bytes out of alignment. That is the signature of I2C
// framing trouble rather than a slow peripheral, and it did not respond to
// giving the part more thinking time (a settle sweep from 200 us to 3000 us
// moved the good-frame rate only from ~36 % to ~47 %). This bus carries six
// devices plus a panel flex; 400 kHz is the same kind of unproven overclock the
// panel bus turned out to be at 80 MHz.
#define I2C_BUS_HZ     100000
// 0x3B, NOT the 0x38 a FocalTech part would answer on. Nothing responds at 0x38
// on this board; polling it emitted ~90 failed transactions a second and buried
// every other line in the serial log.
#define TOUCH_I2C_ADDR 0x3B
// Neither is wired (the vendor's bsp_touch_init is called with tp_rst = -1), so
// there is no interrupt to hang a handler on and no way to reset touch short of
// resetting the board. Polling is the only option — which suits our 10 ms loop.
#define TOUCH_RST_GPIO -1
#define TOUCH_INT_GPIO -1

// ---- Power management / RTC (same shared I2C bus as touch) ------------------
#define PMIC_I2C_ADDR 0x34   // AXP2101 battery/power management  (scan: present)
#define RTC_I2C_ADDR  0x51   // PCF85063 real-time clock          (scan: present)
#define IMU_I2C_ADDR  0x6B   // QMI8658 6-axis IMU (unused)       (scan: present)

// AXP2101 interrupt registers. As on the C6 board, the PWR key is wired to the
// PMIC's PWRKEY pin and NOT to any GPIO, so presses arrive only as IRQ status
// bits that we poll and clear over I2C.
#define PMIC_REG_INTEN2  0x41   // IRQ enable  bank 2
#define PMIC_REG_INTSTS2 0x49   // IRQ status  bank 2 (write 1 to clear)
#define PMIC_IRQ_PKEY_SHORT (1 << 3)
#define PMIC_IRQ_PKEY_LONG  (1 << 2)

// ---- Buttons ---------------------------------------------------------------
// BOOT is the ESP32-S3 strapping button on GPIO0 (active low), per
// 03_button_example. NOTE this differs from the C6 board's GPIO9 — on THIS board
// GPIO9 is the SD card's DAT0 line, so carrying the old pin over would read the
// SD bus as a button.
#define BOOT_BTN_GPIO 0
#define BTN_DEBOUNCE_MS  40
#define BTN_LONG_PRESS_MS 1200

// ---- Audio (ES8311 codec + onboard speaker amp) ----------------------------
// From 04_es8311_example. The codec shares the touch I2C bus and answered the
// boot scan at 0x18.
#define ES8311_I2C_ADDR   0x18   // 7-bit, CE pin low (0x30 is the same part, 8-bit form)
#define ES8311_I2C_PORT   0      // Wire == I2C port 0
// MCLK is 44 on this board, NOT the 12 the plain-3.5 header used — 12 is the
// panel's QSPI chip-select here. GPIO44 is normally UART0 RXD; that is free to
// reuse because the console runs over native USB CDC (ARDUINO_USB_CDC_ON_BOOT),
// not UART0.
#define I2S_MCLK_GPIO     44
#define I2S_BCLK_GPIO     13
#define I2S_DIN_GPIO      14     // codec ASDOUT -> ESP (mic; unused for playback)
#define I2S_WS_GPIO       15     // LRCK
#define I2S_DOUT_GPIO     16     // ESP -> codec DSDIN (speaker)
// There is no software-controllable speaker-amp enable on this board; no demo
// asserts any enable line. audio.cpp therefore skips the expander write it does
// on the C6.
#define SPK_AMP_EN_VIA_EXPANDER 0

// ---- TF / microSD slot (hardware SDMMC host, 1-bit) ------------------------
// From 07_sd_test's SD_MMC.setPins(clk, cmd, d0). Unchanged from the plain-3.5
// port — these three were among the things it got right.
//
// Unlike the C6 — which has no SDMMC peripheral at all and had to bit-bang SPI
// at ~250 kHz — the S3 has a real SDMMC host and Waveshare wires the slot to it.
// Only CLK, CMD and D0 are connected, so this is 1-bit SDMMC; there is no D3,
// which also means SPI-mode SD is NOT available on this board (SPI mode needs D3
// as chip-select).
#define SD_MMC_CLK_GPIO  11
#define SD_MMC_CMD_GPIO  10
#define SD_MMC_D0_GPIO    9

// ---- Backlight / brightness ------------------------------------------------
// A real backlight GPIO driven by LEDC PWM. This is the big behavioural
// difference from the AMOLED board, where brightness was a panel command and
// "black" genuinely cost no power. Here the backlight is what costs power, so
// the idle-dim path matters more and the pixel-shift burn-in mitigation matters
// not at all (an IPS LCD does not burn in) — see display.cpp.
#define LCD_BL_GPIO            6
#define LCD_BL_PWM_HZ          5000
#define LCD_BL_PWM_BITS        10
// Full brightness by default. The C6's AMOLED is bright enough at 200/255 that
// the extra was not worth the power, but this panel reads as too dim there —
// reported on the bench 2026-08-10 — and the vendor's own demos simply drive the
// backlight pin HIGH with no PWM at all, i.e. 100 %. The idle-dim path still
// pulls it down when nothing is happening, which is where the power actually
// gets saved on a backlit LCD.
#define LCD_DEFAULT_BRIGHTNESS 255
