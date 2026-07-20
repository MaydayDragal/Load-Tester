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
// Arduino_GFX draw16bitRGBBitmap expects big-endian 565; if colours look
// swapped on your panel, flip this to 0.
#define LV_COLOR_16_SWAP 1

// ---- Memory ---------------------------------------------------------------
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)
#define LV_MEM_ADR 0

// ---- HAL / tick -----------------------------------------------------------
// Use the Arduino millis() as the tick source.
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF 130

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
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// ---- Widgets used ---------------------------------------------------------
#define LV_USE_TABVIEW 1
#define LV_USE_LIST 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_LABEL 1
#define LV_USE_TEXTAREA 1
#define LV_USE_KEYBOARD 1

#endif  // LV_CONF_H
#endif  // "Content enable"
