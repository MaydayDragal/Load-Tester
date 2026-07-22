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
//   CapacityTest   — battery discharge / capacity engine
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

#include "capacity_test.h"
#include "display.h"
#include "el15_client.h"
#include "el15_controller.h"
#include "resistance_test.h"
#include "ui.h"

static El15Client g_ble;

// The test engines drive the load through the BLE client (the only controller
// on this device).
static ResistanceTest g_test(&g_ble);
static CapacityTest g_batt(&g_ble);

// ---- Status routing --------------------------------------------------------
static void handleStatus(const el15::Status &s) {
  ui::onStatus(s);
  if (g_test.running()) g_test.onStatus(s);
  if (g_batt.running()) g_batt.onStatus(s);
}

// Hardware emergency stop (BOOT button): kill the load NOW, from any screen or
// state, without needing to aim at a touch target. Stops whichever engine is
// running (each engine's stop() leaves the load OFF) and, if the user was
// driving the load manually, pushes an explicit LOAD_OFF too.
static void emergencyStop() {
  bool acted = false;
  if (g_test.running()) { g_test.stop(); acted = true; }
  if (g_batt.running()) { g_batt.stop("Emergency stop"); acted = true; }
  g_ble.setLoad(false);   // manual-load case (and belt-and-braces)
  g_ble.setSetpoint(0);
  Serial.println("[btn] EMERGENCY STOP");
  ui::onEmergencyStop(acted);
}

static void stopAll() {
  // Capture whether a test was mid-run BEFORE stopping it: stopping the test
  // clears the flag, so checking it afterwards would always be false and the
  // safe LOAD_OFF-then-flush teardown would never run (mirrors the Android
  // DeviceCore.disconnect() `wasBusy` capture).
  bool wasBusy = g_test.running() || g_batt.running();
  if (g_test.running()) g_test.stop();
  if (g_batt.running()) g_batt.stop("Disconnected");
  if (wasBusy) g_ble.shutdownAndDisconnect();  // push LOAD_OFF and let it flush
  else g_ble.disconnect();
}

void setup() {
  Serial.begin(115200);
  display::begin();

  // Report why the last reset happened — distinguishes a firmware panic from a
  // brownout or watchdog when chasing spontaneous reboots in the field.
  esp_reset_reason_t rr = esp_reset_reason();
  const char *rrs = rr == ESP_RST_POWERON ? "power-on" : rr == ESP_RST_SW ? "software"
                  : rr == ESP_RST_PANIC ? "PANIC" : rr == ESP_RST_INT_WDT ? "INT-WDT"
                  : rr == ESP_RST_TASK_WDT ? "TASK-WDT" : rr == ESP_RST_WDT ? "WDT"
                  : rr == ESP_RST_BROWNOUT ? "BROWNOUT" : rr == ESP_RST_USB ? "USB"
                  : "other";
  Serial.printf("[boot] reset reason: %s (%d)\n", rrs, (int)rr);

  UiActions actions;
  actions.scan       = []() { ui::clearDevices(); g_ble.startScan(8); };
  actions.connect    = [](const char *addr) { g_ble.connectTo(addr); };
  actions.disconnect = stopAll;
  actions.setMode    = [](int m) { g_ble.setMode(m); };
  actions.setSetpoint= [](float v) { g_ble.setSetpoint(v); };
  actions.setLoad    = [](bool on) { g_ble.setLoad(on); };
  actions.lock       = []() { g_ble.setLock(); };
  actions.setPollRate = [](int ms) {
    g_ble.pollIntervalMs = (uint32_t)ms;   // BLE status poll cadence
    g_test.pollIntervalMs = (uint32_t)ms;  // R-test settle/collect floors adapt
  };
  actions.startRTest = [](float fuse, int steps) {
    if (g_batt.running()) return;   // never let two engines drive the load
    g_test.steps = steps;
    g_test.start(fuse);
  };
  actions.stopRTest  = []() { g_test.stop(); };
  // TODO: persist the last R-Test result to the on-board SD card (SDMMC:
  // CLK=11 CMD=10 D0=18). For now the UI shows the saved-file confirmation but
  // no file is written yet.
  actions.saveRTest  = []() { Serial.println("[rtest] SD save not yet implemented (stub)"); };
  actions.startBatt  = [](float cutoffV, float amps) {
    if (g_test.running()) return;   // never let two engines drive the load
    g_batt.cutoffV = cutoffV;
    g_batt.dischargeA = amps;
    g_batt.start();
  };
  actions.stopBatt   = []() { g_batt.stop(); };
  actions.saveBatt   = []() { Serial.println("[batt] SD save not yet implemented (stub)"); };
  ui::begin(actions);

  g_ble.onState = [](El15Client::State st, const char *info) {
    if (st != El15Client::CONNECTED) {  // safe-off on drop
      if (g_test.running()) g_test.stop();
      if (g_batt.running()) g_batt.stop("Connection lost");
    }
    ui::onConnState((int)st, info);
  };
  g_ble.onStatus = handleStatus;
  g_ble.onDeviceFound = [](const char *addr, const char *name) { ui::onDeviceFound(addr, name); };
  g_ble.begin();

  g_test.onProgress = [](int s, int t, float tgt, float v, float i) { ui::onTestProgress(s, t, tgt, v, i); };
  g_test.onComplete = [](const ResistanceTest::Result &r) { ui::onTestComplete(r); };
  g_test.onError    = [](const char *m) { ui::onTestError(m); };

  g_batt.onProgress = [](float v, float i, float ah, float wh, float temp, uint32_t el, int ph) {
    ui::onBattProgress(v, i, ah, wh, temp, el, ph);
  };
  g_batt.onComplete = [](const CapacityTest::Result &r) {
    Serial.printf("[batt] done: %.3f Ah, %.1f Wh in %lus (%s)\n",
                  r.capacityAh, r.energyWh, (unsigned long)r.durationS, r.stopReason);
    ui::onBattComplete(r);
  };
  g_batt.onError = [](const char *m) {
    Serial.printf("[batt] error: %s\n", m);
    ui::onBattError(m);
  };
}

// ---- Physical buttons ------------------------------------------------------
static void handleButtons() {
  switch (display::pollButtons()) {
    case display::BTN_BOOT_SHORT:
    case display::BTN_BOOT_LONG:
      // BOOT = hardware emergency stop. Wake the screen first so the user sees
      // the acknowledgement, then kill the load.
      if (display::asleep()) display::setSleep(false);
      emergencyStop();
      break;
    case display::BTN_PWR_SHORT:
      display::setSleep(!display::asleep());   // toggle display sleep
      break;
    case display::BTN_PWR_LONG:
      // A held PWR key reads as "wake" only; leave hardware power-off to the
      // PMIC's own long-press handling so we never fight it.
      if (display::asleep()) display::setSleep(false);
      break;
    default: break;
  }
}

void loop() {
  display::loopTick();
  handleButtons();
  g_ble.loopTick();
  g_test.tick();
  g_batt.tick();
  delay(2);
}
