// LVGL 8.x configuration for the EL15 controller.
//
// Trimmed from lv_conf_template.h — anything not set here falls back to LVGL's
// built-in defaults (lv_conf_internal.h). Enables 16-bit colour, the Montserrat
// font sizes the UI uses, and a modest heap.
#if 1  // set this to "1" to enable content

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// ---- Colour ---------------------------------------------------------------
#define LV_COLOR_DEPTH 16
// This panel wants NO byte-swap paired with draw16bitRGBBitmap (verified against
// Waveshare's own lv_conf.h for the C6-Touch-AMOLED-1.8, which sets SWAP 0 and
// uses draw16bitRGBBitmap in its flush). SWAP 1 with that draw call byte-swaps
// every pixel → garbled colours. If you ever switch flushCb to the big-endian
// draw16bitBeRGBBitmap variant, set this back to 1.
// Byte order of the 16-bit pixels LVGL produces.
//
// On the QSPI TFT board this is 1, so LVGL writes big-endian RGB565 and the
// flush can use Arduino_GFX's draw16bitBeRGBBitmap(). That matters a great deal
// on this panel, which needs a whole 300 KB frame pushed on every repaint:
// the little-endian path (writePixels) byte-swaps every one of 153 600 pixels
// on the CPU into an internal staging buffer before transmitting, whereas the
// big-endian path (writeBytes) hands the SPI peripheral our buffer address
// directly. Same pixels, same bus, but one of them reads all 300 KB out of PSRAM
// through the CPU first.
//
// The two settings must stay paired with the matching draw call in display.cpp's
// flushCb, or the red and blue channels swap.
#if defined(BOARD_S3_LCD35B)
#define LV_COLOR_16_SWAP 1
#else
#define LV_COLOR_16_SWAP 0
#endif

// ---- Memory ---------------------------------------------------------------
// Use the system heap instead of a fixed LVGL pool. The redesigned UI builds
// ~200 objects up front, which overran the old 48 KB static pool (lv_obj_create
// returned NULL -> a child created on it dereferenced a null parent and
// panicked). The ESP32-C6 has ample internal RAM, so let LVGL grow on demand.
#define LV_MEM_CUSTOM 1
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
// Put the widget tree in PSRAM. Without this, every LVGL allocation is small
// enough to be served from internal SRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is
// 4096 in the prebuilt libs), which starved the BLE stack badly enough to panic
// on connect. See lv_mem_psram.h for the full account.
#define LV_MEM_CUSTOM_INCLUDE "lv_mem_psram.h"
#define LV_MEM_CUSTOM_ALLOC   lvPsramAlloc
#define LV_MEM_CUSTOM_FREE    lvPsramFree
#define LV_MEM_CUSTOM_REALLOC lvPsramRealloc
#else
// The C6 has no PSRAM; the ordinary heap is the only heap.
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC   malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC realloc
#endif

// ---- HAL / tick -----------------------------------------------------------
// Use the Arduino millis() as the tick source.
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF 130

// Sample the touch panel every 10 ms (default 30) for lower tap latency.
#define LV_INDEV_DEF_READ_PERIOD 10
// Start redrawing sooner after a change (default 30 ms) so press-dim and scroll
// feel more immediate. Redraws are still bounded by their own cost.
// How often LVGL looks for dirty areas and renders a frame.
//
// 16 ms (~60 Hz) suits the C6, whose partial flushes are cheap. It is actively
// harmful on the QSPI TFT board, where the panel demands a whole 300 KB frame per
// repaint and that push measures ~24 ms: asking for a frame every 16 ms requests
// more than the panel can physically accept, so the render loop saturates,
// every refresh blocks for longer than the period, and nothing else gets a look
// in. That does not produce more frames than 33 ms does — it produces the same
// frames plus stutter.
//
// It also starves touch. The touch controller is polled by an LVGL timer on a
// 20 ms period, and a 24 ms blocking flush delays every poll that lands during
// it, which is a good way to make a gesture look jerky.
//
// 33 ms (~30 Hz) is comfortably above the 20 Hz telemetry rate the UI displays,
// so no reading is shown late, and it leaves roughly a quarter of the time free
// for touch, BLE and the test engines.
#if defined(BOARD_S3_LCD35B)
#define LV_DISP_DEF_REFR_PERIOD 33
#else
#define LV_DISP_DEF_REFR_PERIOD 16
#endif

// ---- Feature switches -----------------------------------------------------
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

// ---- Fonts (the UI uses 12/14/16/20/28) -----------------------------------
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_34 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_44 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// ---- Widgets used ---------------------------------------------------------
#define LV_USE_TABVIEW 1
#define LV_USE_LIST 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_LABEL 1
#define LV_USE_TEXTAREA 1
#define LV_USE_BTNMATRIX 1   // required by LV_USE_KEYBOARD
#define LV_USE_KEYBOARD 1
#define LV_USE_CHART 1       // live V/I trend graph on the Home screen

#endif  // LV_CONF_H
#endif  // "Content enable"
