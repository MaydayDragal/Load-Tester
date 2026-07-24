// Persistent settings in NVS (flash), so the controller comes back the way you
// left it instead of resetting to defaults on every power cycle.
//
// One namespace, flat keys, written through a debounced commit: UI callbacks
// change values freely (a slider drag fires dozens of events) and the actual NVS
// write happens from prefs::tick() once they have settled. NVS is flash — an
// undebounced write per slider step would burn erase cycles for nothing.
//
// Loop task only.
#pragma once

#include <stdint.h>

namespace prefs {

// Everything persisted, with the defaults used on a blank device.
struct Data {
  // Display / sound
  uint8_t brightness = 200;
  uint8_t volume = 60;
  bool muted = false;
  uint16_t idleDimS = 120;   // 0 = never; auto-dim after this much idle time
  bool pixelShift = true;    // AMOLED burn-in mitigation

  // Sampling
  uint16_t pollMs = 50;   // 20 Hz — the EL15's practical max fresh-data rate

  // R-Test setup
  float fuseRating = 0;
  uint8_t rtSteps = 8;
  bool fourWire = false;     // 4-wire (Kelvin) probing
  float tareOhm = 0;         // measured lead+contact resistance (2-wire only)

  // Battery test setup
  uint8_t battChem = 0;
  uint8_t battCells = 3;
  float battCutoff = 9.0f;
  bool battCutoffCustom = false;
  float battAmps = 1.0f;

  // Wi-Fi (for NTP time sync only; the radio is otherwise off)
  char ssid[33] = "";
  char pass[65] = "";
  int16_t tzMinutes = 0;     // offset from UTC, in minutes

  // Last device, so a reconnect after a link loss or a crash needs no scan.
  char lastAddr[24] = "";
  uint8_t lastAddrType = 0;
  // Auto-connect to lastAddr on startup (skip the scan+tap each session). Off by
  // default — the firmware should not reach out for a BLE link unprompted.
  bool autoConnect = false;

  // Crash/reboot recovery: non-zero while something is driving the load, so the
  // next boot knows the load may have been left energised.
  enum InFlight : uint8_t { NONE = 0, RTEST = 1, CAPACITY = 2, MANUAL = 3 };
  uint8_t inFlight = NONE;
};

// Load from NVS (or defaults on a blank device). Call once, early in setup().
void begin();

// The live values, read-only.
const Data &get();

// Apply a change and schedule a commit:
//   prefs::change([](prefs::Data &d) { d.brightness = 180; });
Data &mutable_();
void markDirty();
template <typename F>
void change(F &&f) { f(mutable_()); markDirty(); }

// Commit pending changes once they have settled. Call from loop().
void tick();

// Write pending changes right now (before a deliberate reboot).
void flush();

// Crash/reboot recovery flag. Both commit synchronously — the whole point is
// that the flag survives a panic one millisecond later.
void armInFlight(uint8_t what);
void clearInFlight();

}  // namespace prefs
