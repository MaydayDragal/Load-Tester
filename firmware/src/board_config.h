// Board configuration for the Waveshare ESP32-C6-Touch-AMOLED-1.8.
//
//   Panel : 1.8" AMOLED, 368 x 448, SH8601 controller over QSPI
//   Touch : FT3168 capacitive, I2C
//   MCU   : ESP32-C6 (RISC-V, BLE 5)
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ VERIFY THESE PINS against Waveshare's own Arduino demo for THIS board     │
// │ before flashing. Waveshare occasionally revises pin maps between board    │
// │ revisions, and several control lines (reset / panel power) may sit behind │
// │ the on-board TCA9554 I/O expander rather than a direct GPIO. The values   │
// │ below follow Waveshare's published ESP32-C6-Touch-AMOLED-1.8 example;     │
// │ cross-check them with the wiki page and the demo's config header.         │
// │   https://www.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-1.8                │
// └─────────────────────────────────────────────────────────────────────────┘
#pragma once

// ---- Panel geometry --------------------------------------------------------
#define LCD_WIDTH   368
#define LCD_HEIGHT  448

// ---- AMOLED QSPI bus (SH8601) ---------------------------------------------
// Verified against Waveshare's own pin_config.h for this exact board
// (waveshareteam/ESP32-C6-Touch-AMOLED-1.8, Arduino-v3.3.5/libraries/Mylibrary).
#define LCD_QSPI_CS    5
#define LCD_QSPI_SCK   0
#define LCD_QSPI_D0    1   // SDIO0
#define LCD_QSPI_D1    2   // SDIO1
#define LCD_QSPI_D2    3   // SDIO2
#define LCD_QSPI_D3    4   // SDIO3
// Panel reset is NOT a GPIO on this board — it is released via the TCA9554
// expander (see below), so the SH8601 driver gets GFX_NOT_DEFINED (-1).
#define LCD_RST_GPIO   -1
#define LCD_TE_GPIO    -1

// The panel reset/enable lines are behind the on-board TCA9554 I/O expander at
// 0x20. Waveshare's demo drives expander bits 4 & 5 high to power/enable the
// panel and release it from reset before gfx->begin(); display.cpp does the same.
#define LCD_RST_VIA_EXPANDER 1
#define IO_EXPANDER_ADDR     0x20
#define LCD_EXPANDER_PWR_BITS ((1 << 4) | (1 << 5))

// ---- Touch (FT3168, I2C) ---------------------------------------------------
// Shared I2C bus (touch + TCA9554 + QMI8658 + PCF85063 + AXP2101): SDA=8, SCL=7.
#define TOUCH_I2C_SDA  8
#define TOUCH_I2C_SCL  7
#define TOUCH_I2C_ADDR 0x38
#define TOUCH_RST_GPIO -1
#define TOUCH_INT_GPIO 15

// ---- Backlight / brightness ------------------------------------------------
// AMOLED brightness is a controller command (0x51), handled in display.cpp;
// no PWM backlight pin. Range 0..255.
#define LCD_DEFAULT_BRIGHTNESS 200
