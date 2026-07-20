// Display + touch glue: brings up the SH8601 AMOLED (via Arduino_GFX) and the
// FT3168 capacitive touch (I2C), and wires both into LVGL. Keeps all the
// board-specific bring-up in one place; the UI layer only sees LVGL.
#pragma once

#include <lvgl.h>

namespace display {

// Initialise panel, touch, and LVGL. Call once from setup().
void begin();

// Pump LVGL timers + the FT3168 read. Call frequently from loop().
void loopTick();

// AMOLED brightness, 0..255 (SH8601 write-brightness command).
void setBrightness(uint8_t level);

}  // namespace display
