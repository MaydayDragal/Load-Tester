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
uint8_t getBrightness();

// System info for the Settings screen. I2C reads — call from the loop task only.
// chargeState: 1-3 = charging phases, 4 = charge done, else not charging.
bool batteryStats(int &percent, int &milliVolt, int &chargeState, bool &present);
// Returns false when the RTC is absent or its oscillator-stop flag says the
// time was never set.
bool rtcTime(int &year, int &mon, int &day, int &hour, int &min, int &sec);

// ---- Physical buttons ------------------------------------------------------
// BOOT is a GPIO; PWR is read from the PMIC's latched key IRQs. Poll from the
// loop task; each event is reported exactly once.
enum ButtonEvent { BTN_NONE, BTN_BOOT_SHORT, BTN_BOOT_LONG, BTN_PWR_SHORT, BTN_PWR_LONG };
ButtonEvent pollButtons();

// Display sleep: blanks the panel (AMOLED black draws no current) and makes
// the touch layer inert so blind taps can't change anything. Any button press,
// or a tap on the dark screen, wakes it.
void setSleep(bool on);
bool asleep();

}  // namespace display
