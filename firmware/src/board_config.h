// Board configuration for the Waveshare ESP32-C6-Touch-AMOLED-1.8.
//
//   Panel : 1.8" AMOLED, 368 x 448, QSPI. Waveshare's spec names the driver
//           CO5300; we drive it with Arduino_GFX's Arduino_SH8601, whose
//           command set this controller accepts (verified working on hardware).
//   Touch : capacitive over I2C. Waveshare's spec names CST820; the part on
//           this unit answers on the FocalTech (FT3168/FT6x36) register map.
//           display.cpp initialises for both — see touchInit().
//   MCU   : ESP32-C6 (RISC-V, Wi-Fi 6 / BLE 5 / 802.15.4)
//   RAM   : 512 KB on-chip HP SRAM. There is NO PSRAM on this board, which is
//           why LVGL uses a partial (1/8 frame) draw buffer.
//   Also on board, currently unused by this firmware: QMI8658 6-axis IMU,
//   RTC backup-battery pads.
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

// ---- Power management / RTC (same shared I2C bus as touch) ------------------
#define PMIC_I2C_ADDR 0x34   // AXP2101 battery/power management
#define RTC_I2C_ADDR  0x51   // PCF85063 real-time clock

// AXP2101 interrupt registers. The PWR key is wired to the PMIC's PWRKEY pin,
// not to a GPIO, so its presses arrive as IRQ status bits we poll and clear.
#define PMIC_REG_INTEN2  0x41   // IRQ enable  bank 2
#define PMIC_REG_INTSTS2 0x49   // IRQ status  bank 2 (write 1 to clear)
#define PMIC_IRQ_PKEY_SHORT (1 << 3)
#define PMIC_IRQ_PKEY_LONG  (1 << 2)

// ---- Buttons ---------------------------------------------------------------
// BOOT is the standard ESP32-C6 strapping button on GPIO9: pulled up, pressed
// pulls it LOW. Safe to read at runtime (it only matters during reset).
#define BOOT_BTN_GPIO 9
#define BTN_DEBOUNCE_MS  40
#define BTN_LONG_PRESS_MS 1200

// ---- Audio (ES8311 codec + onboard speaker amp) ----------------------------
// Verified against waveshareteam/ESP32-C6-Touch-AMOLED-1.8 pin_config.h +
// the 15_ES8311 example. The codec shares the touch I2C bus (SDA 8 / SCL 7);
// the speaker amplifier's power-enable is TCA9554 expander bit 7 (same 0x20
// expander as the panel), driven high to un-mute the speaker.
#define ES8311_I2C_ADDR   0x18   // CE pin low
#define ES8311_I2C_PORT   0      // Wire == I2C port 0
#define I2S_MCLK_GPIO     19
#define I2S_BCLK_GPIO     20
#define I2S_DIN_GPIO      21     // codec ASDOUT -> ESP (mic; unused for playback)
#define I2S_WS_GPIO       22
#define I2S_DOUT_GPIO     23     // ESP -> codec DSDIN (speaker)
#define SPK_AMP_EN_BIT    (1 << 7)  // expander bit that powers the speaker amp

// ---- TF / microSD slot (SPI mode, SHARED with the panel's SPI2 host) --------
// Verified against waveshareteam/ESP32-C6-Touch-AMOLED-1.8 pin_config.h, which
// names them SDMMC_* out of habit — but the ESP32-C6 has NO SDMMC host
// (soc_caps.h: no SOC_SDMMC_HOST_SUPPORTED), so the slot can only run in SPI
// mode: CMD = MOSI, DATA/D0 = MISO, CLK = SCK.
//
// The C6 also has only ONE general-purpose SPI host (SOC_SPI_PERIPH_NUM = 2,
// and SPI1 is the flash controller), and the AMOLED owns it in QSPI mode. The
// card therefore lives on the SAME host as a second device, and sd_card.cpp
// switches the shared clock/data signals between the panel's pins and these via
// the GPIO matrix around every card access. Nothing may draw while the card is
// mounted — see the routing note in sd_card.cpp.
#define SD_SPI_SCK   11
#define SD_SPI_MOSI  10   // card CMD
#define SD_SPI_MISO  18   // card DAT0
#define SD_SPI_CS     6
// 20 MHz is the ESP-IDF default (SDMMC_FREQ_DEFAULT). Card init always probes
// at 400 kHz first, so this only affects the data phase.
#define SD_SPI_FREQ_KHZ 20000

// ---- Backlight / brightness ------------------------------------------------
// AMOLED brightness is a controller command (0x51), handled in display.cpp;
// no PWM backlight pin. Range 0..255.
#define LCD_DEFAULT_BRIGHTNESS 200
