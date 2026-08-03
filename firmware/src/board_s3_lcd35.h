// Board configuration for the Waveshare ESP32-S3-Touch-LCD-3.5.
//
// Selected by BOARD_S3_LCD35 — see board_config.h. Do not include directly.
// Waveshare's "-C" SKU is this same board bundled with an OV5640 camera module;
// the board itself is identical, and this firmware does not use the camera.
// (The separate "3.5B" product is NOT this board — it drives its panel over
// QSPI. These pins do not apply to it.)
//
//   Panel : 3.5" IPS LCD, 320 x 480, ST7796 over ordinary 4-wire SPI.
//           BGR element order with colour inversion ON — Arduino_GFX gets this
//           right when the ST7796 is constructed with ips = true.
//   Touch : FT6336 capacitive over I2C at 0x38 (FocalTech FT6x36 register map —
//           the same family the C6 board's part answers on, so display.cpp's
//           existing touch driver works unchanged). INT and RST are NOT wired to
//           anything, so the controller must be POLLED, which is what we do.
//   MCU   : ESP32-S3 (Xtensa dual-core, 240 MHz, Wi-Fi 4 / BLE 5)
//   RAM   : 512 KB on-chip SRAM PLUS 8 MB octal PSRAM. Unlike the C6 board there
//           is memory to spare, so LVGL gets full double buffers in PSRAM and
//           NimBLE no longer competes with the UI for contiguous internal heap.
//   Also on board, unused by this firmware: QMI8658 6-axis IMU (0x6B), camera
//   interface (OV2640/OV5640, rails from the AXP2101's BLDO1/BLDO2).
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ NOT YET VERIFIED ON HARDWARE. Every value below was read out of          │
// │ Waveshare's own demo sources (github.com/waveshareteam/                  │
// │ ESP32-S3-Touch-LCD-3.5 — ESP-IDF/01_factory/main/esp_*_port.cpp and the  │
// │ Arduino/examples/*), NOT from a schematic and NOT from a running board.  │
// │ The vendor repo ships no schematic and no pin table. Confirm against     │
// │ real hardware before trusting a bench result from this target.           │
// │   https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.5                  │
// └─────────────────────────────────────────────────────────────────────────┘
#pragma once

// ---- Capability flags (what display.cpp / sd_card.cpp compile in) ----------
#define BOARD_NAME              "ESP32-S3-Touch-LCD-3.5"
#define BOARD_PANEL_QSPI_AMOLED 0
#define BOARD_PANEL_SPI_TFT     1   // Arduino_ST7796 over Arduino_ESP32SPI
#define BOARD_BACKLIGHT_PWM     1   // real backlight GPIO, driven by LEDC
#define BOARD_SD_SDMMC          1   // hardware SDMMC host, 1-bit
#define BOARD_SD_SOFT_SPI       0
#ifndef BOARD_HAS_PSRAM
#define BOARD_HAS_PSRAM         1   // 8 MB octal PSRAM (platformio.ini also sets this)
#endif

// ---- Panel geometry --------------------------------------------------------
#define LCD_WIDTH   320
#define LCD_HEIGHT  480

// ---- LCD SPI bus (ST7796) --------------------------------------------------
// From ESP-IDF/01_factory/main/esp_3inch5_lcd_port.cpp and the Arduino demos
// (08_gfx_helloworld, 11_lvgl_arduino_v8), which agree exactly.
#define LCD_SPI_MOSI   1
#define LCD_SPI_MISO   2    // panel SDO; Arduino demos wire it, the IDF port sets NC
#define LCD_SPI_SCK    5
#define LCD_DC_GPIO    3
// CS is genuinely NOT brought out — the panel is the only device on SPI2 and its
// chip-select is tied asserted on the board. It is NOT behind the expander.
#define LCD_CS_GPIO    -1
// RST is likewise not a GPIO: the reset line hangs off the TCA9554 expander and
// is pulsed over I2C before the panel is initialised (see below).
#define LCD_RST_GPIO   -1
#define LCD_TE_GPIO    -1
// Waveshare's ESP-IDF port clocks this panel at 80 MHz; their Arduino demos take
// Arduino_GFX's 40 MHz default and are the configuration actually proven to
// render on this board. We start at the proven 40 MHz. Raising this to 80000000
// is the first thing to try if redraws feel slow — but verify on hardware, since
// an over-clocked SPI panel fails as visual corruption rather than a clean error.
#define LCD_SPI_HZ     40000000

// The panel's reset line is TCA9554 bit 1, ACTIVE LOW: hold low, then release
// high, then let it settle before the controller is initialised. This is a
// different shape from the C6 board (which holds two bits HIGH to power a panel
// rail), so display.cpp keys off LCD_RST_EXPANDER_ACTIVE_LOW rather than reusing
// the C6's hold-high path.
#define LCD_RST_VIA_EXPANDER 1
#define LCD_RST_EXPANDER_ACTIVE_LOW 1
#define IO_EXPANDER_ADDR     0x20
#define LCD_EXPANDER_RST_BIT (1 << 1)
// Nothing else on this expander has a documented purpose. Vendor code touches
// only bit 1 (above) and, in one demo, drives bit 0 high with no explanation;
// the SD card and the audio path both work without it. Bits 2-7 are never
// configured. Do not invent uses for them without a schematic.

// ---- Touch (FT6336, I2C) ---------------------------------------------------
// Shared I2C bus (touch + TCA9554 + AXP2101 + ES8311 + PCF85063 + QMI8658, and
// the camera's SCCB on the -C SKU): SDA=8, SCL=7.
#define TOUCH_I2C_SDA  8
#define TOUCH_I2C_SCL  7
#define TOUCH_I2C_ADDR 0x38
// Both are explicitly not-connected in the vendor port (GPIO_NUM_NC): there is
// no touch interrupt to hang a handler on and no way to reset the controller
// short of resetting the board. Polling is not a choice here, it is the only
// option — which suits our 10 ms polled touch loop.
#define TOUCH_RST_GPIO -1
#define TOUCH_INT_GPIO -1

// ---- Power management / RTC (same shared I2C bus as touch) ------------------
#define PMIC_I2C_ADDR 0x34   // AXP2101 battery/power management
#define RTC_I2C_ADDR  0x51   // PCF85063 real-time clock
#define IMU_I2C_ADDR  0x6B   // QMI8658 6-axis IMU (unused by this firmware)

// AXP2101 interrupt registers. As on the C6 board, the PWR key is wired to the
// PMIC's PWRKEY pin and NOT to any GPIO (the vendor's IRQ-pin attach is
// commented out and their examples hardcode CONFIG_PMU_IRQ -1), so presses
// arrive only as IRQ status bits that we poll and clear over I2C.
#define PMIC_REG_INTEN2  0x41   // IRQ enable  bank 2
#define PMIC_REG_INTSTS2 0x49   // IRQ status  bank 2 (write 1 to clear)
#define PMIC_IRQ_PKEY_SHORT (1 << 3)
#define PMIC_IRQ_PKEY_LONG  (1 << 2)

// ---- Buttons ---------------------------------------------------------------
// BOOT is the ESP32-S3 strapping button on GPIO0 (active low), confirmed as the
// board's only button in 01_factory. NOTE this differs from the C6 board's
// GPIO9 — on THIS board GPIO9 is the SD card's DAT0 line, so carrying the old
// pin over would read the SD bus as a button.
#define BOOT_BTN_GPIO 0
#define BTN_DEBOUNCE_MS  40
#define BTN_LONG_PRESS_MS 1200

// ---- Audio (ES8311 codec + onboard speaker amp) ----------------------------
// From ESP-IDF/01_factory/main/esp_es8311_port.cpp. The codec shares the touch
// I2C bus. Direction note: the vendor's Chinese comments on DOUT/DIN are
// swapped; the authoritative mapping is their std_cfg.gpio_cfg assignment —
// GPIO16 is the ESP's transmit line (to the codec DAC, i.e. the speaker) and
// GPIO14 is receive (from the codec ADC, i.e. the mic).
#define ES8311_I2C_ADDR   0x18   // 7-bit, CE pin low (0x30 is the same part, 8-bit form)
#define ES8311_I2C_PORT   0      // Wire == I2C port 0
#define I2S_MCLK_GPIO     12
#define I2S_BCLK_GPIO     13
#define I2S_DIN_GPIO      14     // codec ASDOUT -> ESP (mic; unused for playback)
#define I2S_WS_GPIO       15
#define I2S_DOUT_GPIO     16     // ESP -> codec DSDIN (speaker)
// There is NO software-controllable speaker-amp enable on this board. The vendor
// port sets es8311_cfg.pa_pin = GPIO_NUM_NC while still declaring a 5 V PA, and
// no demo asserts any enable line: the amplifier is powered permanently in
// hardware. audio.cpp therefore skips the expander write it does on the C6.
#define SPK_AMP_EN_VIA_EXPANDER 0

// ---- TF / microSD slot (hardware SDMMC host, 1-bit) ------------------------
// From ESP-IDF/01_factory/main/esp_sdcard_port.cpp, matching the Arduino
// SD_MMC.setPins(clk, cmd, d0) demo.
//
// Unlike the C6 — which has no SDMMC peripheral at all and had to bit-bang SPI
// at ~250 kHz — the S3 has a real SDMMC host and Waveshare wires the slot to it.
// Only CLK, CMD and D0 are connected, so this is 1-bit SDMMC; there is no D3,
// which also means SPI-mode SD is NOT available on this board (SPI mode needs
// D3 as chip-select). sd_card.cpp uses the Arduino SD_MMC driver here, which is
// on the order of a hundred times faster than the C6's software SPI.
#define SD_MMC_CLK_GPIO  11
#define SD_MMC_CMD_GPIO  10
#define SD_MMC_D0_GPIO    9

// ---- Backlight / brightness ------------------------------------------------
// A real backlight GPIO driven by LEDC PWM (the vendor uses channel 0 / timer 1
// at 5 kHz, 10-bit). This is the big behavioural difference from the AMOLED
// board, where brightness was a panel command and "black" genuinely cost no
// power. Here the backlight is what costs power, so the idle-dim path matters
// more and the pixel-shift burn-in mitigation matters not at all (an IPS LCD
// does not burn in) — see display.cpp.
#define LCD_BL_GPIO            6
#define LCD_BL_PWM_HZ          5000
#define LCD_BL_PWM_BITS        10
#define LCD_DEFAULT_BRIGHTNESS 200

// ---- Free GPIO -------------------------------------------------------------
// For anyone adding hardware: GPIO4 is the ONLY general-purpose pin not spoken
// for on this board. Dropping the camera would additionally free 17, 18, 21 and
// 38-42, 45-48. Everything else is committed (panel, I2C, SD, I2S, PSRAM, USB,
// UART0).
