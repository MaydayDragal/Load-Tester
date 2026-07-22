// EL15 Load Control — standalone ESP32-C6 firmware.
//
// Turns a Waveshare ESP32-C6-Touch-AMOLED-1.8 into a self-contained controller
// for the ALIENTEK EL15 electronic load: it is the BLE central (no phone
// needed), renders the instrument UI on the AMOLED touch panel, and runs the
// same fuse-aware resistance test as the Android app.
//
// Architecture mirrors the Android app:
//   el15_protocol  — wire protocol (packet parse + command frames)
//   El15Client     — BLE central transport (scan/connect/subscribe/poll)
//   ResistanceTest — fuse-aware sweep engine
//   ui             — LVGL screens
//
// There is deliberately no on-device simulator: to bench-test without load
// hardware, run the Android "EL15 Load Simulator" app (simulator/ in this
// repo), which impersonates the load over a real BLE link — so the firmware
// always exercises its actual transport path.
//
// main.cpp owns the objects and routes events between them, standing in for the
// Android DeviceCore.

#include <Arduino.h>

#include "display.h"
#include "el15_client.h"
#include "resistance_test.h"
#include "ui.h"

static El15Client g_ble;
static ResistanceTest g_test(&g_ble);

// ---- Status routing --------------------------------------------------------
static void handleStatus(const el15::Status &s) {
  ui::onStatus(s);
  if (g_test.running()) g_test.onStatus(s);
}

static void disconnectAll() {
  // Capture whether a test was mid-run BEFORE stopping it: stopping the test
  // clears the flag, so checking it afterwards would always be false and the
  // safe LOAD_OFF-then-flush teardown would never run (mirrors the Android
  // DeviceCore.disconnect() `wasBusy` capture).
  bool wasBusy = g_test.running();
  if (wasBusy) g_test.stop();
  if (wasBusy) g_ble.shutdownAndDisconnect();  // push LOAD_OFF and let it flush
  else g_ble.disconnect();
}

void setup() {
  Serial.begin(115200);
  display::begin();

  UiActions actions;
  actions.scan       = []() { ui::clearDevices(); g_ble.startScan(8); };
  actions.connect    = [](const char *addr) { g_ble.connectTo(addr); };
  actions.disconnect = disconnectAll;
  actions.setMode    = [](int m) { g_ble.setMode(m); };
  actions.setSetpoint= [](float v) { g_ble.setSetpoint(v); };
  actions.setLoad    = [](bool on) { g_ble.setLoad(on); };
  actions.lock       = []() { g_ble.setLock(); };
  actions.startRTest = [](float fuse, int steps) {
    g_test.steps = steps;
    g_test.start(fuse);
  };
  actions.stopRTest  = []() { g_test.stop(); };
  // TODO: persist the last R-Test result to the on-board SD card (SDMMC:
  // CLK=11 CMD=10 D0=18). For now the UI shows the saved-file confirmation but
  // no file is written yet.
  actions.saveRTest  = []() { Serial.println("[rtest] SD save not yet implemented (stub)"); };
  ui::begin(actions);

  g_ble.onState = [](El15Client::State st, const char *info) {
    if (st != El15Client::CONNECTED && g_test.running()) g_test.stop();  // safe-off on drop
    ui::onConnState((int)st, info);
  };
  g_ble.onStatus = handleStatus;
  g_ble.onDeviceFound = [](const char *addr, const char *name) { ui::onDeviceFound(addr, name); };
  g_ble.begin();

  g_test.onProgress = [](int s, int t, float tgt, float v, float i) { ui::onTestProgress(s, t, tgt, v, i); };
  g_test.onComplete = [](const ResistanceTest::Result &r) { ui::onTestComplete(r); };
  g_test.onError    = [](const char *m) { ui::onTestError(m); };
}

void loop() {
  display::loopTick();
  g_ble.loopTick();
  g_test.tick();
  delay(2);
}
