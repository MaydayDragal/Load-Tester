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
#define LCD_QSPI_CS    14
#define LCD_QSPI_SCK   1
#define LCD_QSPI_D0    5
#define LCD_QSPI_D1    6
#define LCD_QSPI_D2    7
#define LCD_QSPI_D3    8
// -1 if reset is driven through the TCA9554 expander (see LCD_RST_VIA_EXPANDER).
#define LCD_RST_GPIO   21
#define LCD_TE_GPIO    -1

// Some board revisions gate the panel reset / enable behind a TCA9554 at 0x20.
// If your board does, set this to 1 and adjust the expander bit in display.cpp.
#define LCD_RST_VIA_EXPANDER 0
#define IO_EXPANDER_ADDR     0x20

// ---- Touch (FT3168, I2C) ---------------------------------------------------
#define TOUCH_I2C_SDA  47
#define TOUCH_I2C_SCL  48
#define TOUCH_I2C_ADDR 0x38
#define TOUCH_RST_GPIO -1
#define TOUCH_INT_GPIO -1

// ---- Backlight / brightness ------------------------------------------------
// AMOLED brightness is a controller command (0x51), handled in display.cpp;
// no PWM backlight pin. Range 0..255.
#define LCD_DEFAULT_BRIGHTNESS 200
