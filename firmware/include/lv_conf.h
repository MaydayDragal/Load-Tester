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
#define LV_COLOR_16_SWAP 0

// ---- Memory ---------------------------------------------------------------
// Use the system heap instead of a fixed LVGL pool. The redesigned UI builds
// ~200 objects up front, which overran the old 48 KB static pool (lv_obj_create
// returned NULL -> a child created on it dereferenced a null parent and
// panicked). The ESP32-C6 has ample internal RAM, so let LVGL grow on demand.
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC   malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC realloc

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
#define LV_DISP_DEF_REFR_PERIOD 16

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
