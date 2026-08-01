// LVGL user interface for the standalone controller. The UI is decoupled from
// BLE/simulator specifics: main.cpp wires a set of action callbacks, and the UI
// pushes live data in through the on*() entry points. Core-first parity with
// the Android app: connect/scan, live monitor, manual control, resistance test.
#pragma once

#include <stddef.h>

#include <functional>
#include "capacity_test.h"
#include "el15_protocol.h"
#include "resistance_test.h"

struct UiActions {
  std::function<void()> scan;
  std::function<void(const char *addr)> connect;
  std::function<void()> disconnect;
  std::function<void(int mode)> setMode;
  std::function<void(float value)> setSetpoint;
  std::function<void(bool on)> setLoad;
  std::function<void()> lock;
  std::function<void(int ms)> setPollRate;   // status sampling interval
  // fourWire/tareOhm describe the probe wiring: 4-wire (Kelvin) sensing needs
  // no lead correction, 2-wire subtracts the measured lead+contact tare.
  // Continuous triangular sweep: startA -> maxA -> startA over sweepS seconds.
  // maxA == 0 asks the engine to use the whole headroom the fuse allows.
  std::function<void(float fuse, float startA, float maxA, uint32_t sweepS,
                     bool fourWire, float tareOhm)> startRTest;
  std::function<void()> stopRTest;
  // ratedAh is the pack's nameplate capacity, or 0 when the user did not enter
  // one — state-of-health is then suppressed rather than guessed at. chem/cells
  // select the discharge-curve model behind the time-remaining estimate (see
  // battery_model.h); a chemistry with no curve leaves the engine on its
  // rated-capacity fallback.
  std::function<void(float cutoffV, float amps, float ratedAh, int chem, int cells)> startBatt;
  std::function<void()> stopBatt;
  // Re-energise a discharge that was paused (controller battery, link loss).
  // False = there was nothing paused to resume.
  std::function<bool()> resumeBatt;
  // Engine's estimate of seconds of discharge left; 0 when it has none.
  std::function<uint32_t()> battRemainingS;
  // Live state of charge in percent, or < 0 before the model has established
  // itself (it needs a few seconds of discharge to measure the pack's internal
  // resistance first). Always shown as an approximation — see battery_model.h.
  std::function<float()> battSocPct;
  // True when battRemainingS() comes from the discharge curve rather than from
  // the nameplate rating alone. The UI labels which, because they are not
  // equally trustworthy.
  std::function<bool()> battEtaFromCurve;

  // SD card. All three block for up to ~2 s (card init dominates) and report
  // honestly: true = written, and `msg` holds the file name; false = nothing was
  // saved, and `msg` holds the reason to show the user. The card runs on its own
  // bit-banged SPI pins (sd_card.cpp) so a redraw no longer conflicts with it,
  // but the call still blocks the loop task — so the UI must render any
  // "Saving..." state BEFORE calling.
  std::function<bool(char *msg, size_t len)> saveRTest;   // last R-Test result
  std::function<bool(char *msg, size_t len)> saveBatt;    // last capacity result
  std::function<bool(char *msg, size_t len)> sdInfo;      // card present + size
  std::function<void()> syncClock;   // start a Wi-Fi NTP sync of the RTC
  std::function<bool()> scanWifi;    // start a Wi-Fi scan (false = radio busy)
};

namespace ui {

void begin(const UiActions &actions);

// Live data in (call from loop context / device callbacks).
void onStatus(const el15::Status &s);
void onConnState(int state, const char *info);   // El15Client::State
void onDeviceFound(const char *address, const char *name);
void clearDevices();

// Live sweep progress. `r` is the running resistance estimate and is only
// meaningful once `rValid` is true (the ramp has to cover some current span
// before a slope means anything).
void onTestProgress(float elapsedS, float totalS, float target,
                    float v, float i, float r, bool rValid);
void onTestComplete(const ResistanceTest::Result &r);
void onTestError(const char *msg);

// phase: 1 = discharging, 2 = resting, 3 = paused.
void onBattProgress(float v, float i, float ah, float wh, float temp, uint32_t elapsedS, int phase);
void onBattComplete(const CapacityTest::Result &r);
void onBattError(const char *msg);

// The discharge was suspended (or un-suspended) without ending the test.
// `reason` is a static string while paused, nullptr on resume.
void onBattPaused(bool paused, const char *reason);

// Hardware emergency stop (BOOT button): show the acknowledgement banner and
// unstick any running-test view.
void onEmergencyStop(bool wasTestRunning);

// Link-loss supervisor / crash recovery, in the full-width banner.
//  - `resolved` = the load is confirmed off; the banner goes green and can be
//    dismissed with a tap.
//  - otherwise it is a live warning (red, cannot be dismissed by tapping) and
//    the screen is force-woken, because this is the one thing the user must see.
// The banner is painted immediately: the supervisor blocks for seconds at a
// time while it reconnects.
void onGuardAlert(const char *title, const char *msg, bool resolved);

// Offer boot-time recovery after a crash left the load energised. Tapping the
// banner runs `action` (reconnect + force LOAD OFF).
void offerRecovery(const char *msg, std::function<void()> action);

// Dismiss any guard banner (locked warning or armed recovery offer). Call when
// the user takes manual control (scan/connect) — the guard stands down and its
// stale banner must not stay locked on screen suppressing later alerts.
void clearGuardBanner();

// The controller's OWN battery hit the critical threshold and the load was
// force-stopped before brownout. `wasHot` = something was actually sinking.
void onPowerCritical(int percent, bool wasHot);

// A load-safe power-off is in progress (load stopped, about to cut power).
void onPoweringOff();

// Wi-Fi/NTP progress for the Settings clock card. `state` is net::State;
// 3 = done, 4 = failed.
void onNetProgress(int state, const char *text);

// Result of a Wi-Fi scan: `n` networks (already deduped, strongest-first),
// `ssids[i]`/`rssi[i]` valid for i in [0,n). `err` non-null = scan failed and
// `n` is 0. Populates the network-picker overlay.
void onWifiScanResult(const char *const *ssids, const int *rssi, int n, const char *err);

}  // namespace ui
