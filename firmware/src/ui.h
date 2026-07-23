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
  std::function<void(float fuse, int steps, bool fourWire, float tareOhm)> startRTest;
  std::function<void()> stopRTest;
  std::function<void(float cutoffV, float amps)> startBatt;
  std::function<void()> stopBatt;

  // SD card. All three block for up to ~2 s (card init) and report honestly:
  // true = written, and `msg` holds the file name; false = nothing was saved,
  // and `msg` holds the reason to show the user. Nothing may draw meanwhile —
  // the card shares the panel's SPI bus (sd_card.cpp) — so the UI must render
  // any "Saving..." state BEFORE calling.
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

void onTestProgress(int step, int total, float target, float v, float i);
void onTestComplete(const ResistanceTest::Result &r);
void onTestError(const char *msg);

void onBattProgress(float v, float i, float ah, float wh, float temp, uint32_t elapsedS, int phase);
void onBattComplete(const CapacityTest::Result &r);
void onBattError(const char *msg);

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
