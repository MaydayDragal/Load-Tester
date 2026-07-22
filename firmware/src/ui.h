// LVGL user interface for the standalone controller. The UI is decoupled from
// BLE/simulator specifics: main.cpp wires a set of action callbacks, and the UI
// pushes live data in through the on*() entry points. Core-first parity with
// the Android app: connect/scan, live monitor, manual control, resistance test.
#pragma once

#include <functional>
#include "capacity_test.h"
#include "el15_protocol.h"
#include "resistance_test.h"

struct UiActions {
  std::function<void()> scan;
  std::function<void(const char *addr)> connect;
  std::function<void()> startDemo;
  std::function<void()> disconnect;
  std::function<void(int mode)> setMode;
  std::function<void(float value)> setSetpoint;
  std::function<void(bool on)> setLoad;
  std::function<void()> lock;
  std::function<void(int ms)> setPollRate;   // status sampling interval
  std::function<void(float fuse, int steps)> startRTest;
  std::function<void()> stopRTest;
  std::function<void()> saveRTest;   // persist the last R-Test result (e.g. to SD)
  std::function<void(float cutoffV, float amps)> startBatt;
  std::function<void()> stopBatt;
  std::function<void()> saveBatt;    // persist the last capacity result (stub until SD)
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

}  // namespace ui
