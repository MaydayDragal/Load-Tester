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
// Set the RTC (local time, 24-hour). Clears the oscillator-stop flag, so
// rtcTime() starts returning true. False = the write did not reach the part.
bool setRtcTime(int year, int mon, int day, int hour, int min, int sec);

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

// ---- AMOLED burn-in mitigation ---------------------------------------------
// A bench instrument shows the same static labels for hours; on an OLED that
// burns in permanently. Both defences are independent of the UI layer.

// Pixel shift: creep the whole layout around a 3x3 px box so no edge sits on
// the same subpixel. On by default; costs one redraw every 90 s.
void setPixelShift(bool on);
bool pixelShift();

// Dim after this many seconds without a touch or button press, then blank at
// 5x that. 0 disables both. The user's brightness is restored on any input.
void setIdleDim(uint16_t seconds);
uint16_t idleDim();

// Suppress the blank step (not the dim) — set while a test is running so an
// unattended run keeps a visible, dimmed screen.
void inhibitSleep(bool on);

// Reset the idle timer and undo dim/sleep. Called by the touch driver; call it
// for any other interaction that should count as "the user is here".
void noteActivity();

// ---- Low-memory mode -------------------------------------------------------
// Temporarily shrink the LVGL draw buffer to free ~70 KB for the Wi-Fi stack
// (esp_wifi_init needs ~50 KB and this board has no PSRAM). The UI keeps working
// but renders in smaller chunks. Turn on immediately before a Wi-Fi scan/sync
// and off the moment it finishes. Idempotent.
void setLowMemMode(bool on);
bool lowMemMode();

}  // namespace display
