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
//   El15Simulator  — built-in demo load (no hardware needed)
//   ResistanceTest — fuse-aware sweep engine
//   ui             — LVGL screens
//
// main.cpp owns the objects and routes events between them, standing in for the
// Android DeviceCore.

#include <Arduino.h>

#include "display.h"
#include "el15_client.h"
#include "el15_controller.h"
#include "resistance_test.h"
#include "ui.h"

static El15Client g_ble;
static El15Simulator g_sim;
static bool g_demo = false;

// The active controller: simulator when in demo mode, else the BLE client.
static El15Controller *activeController() {
  return g_demo ? (El15Controller *)&g_sim : (El15Controller *)&g_ble;
}

// A thin controller shim so the test engine always talks to whichever transport
// is active, even if the user switches between demo and BLE.
struct RoutingController : El15Controller {
  void setMode(int m) override { activeController()->setMode(m); }
  void setSetpoint(float v) override { activeController()->setSetpoint(v); }
  void setLoad(bool on) override { activeController()->setLoad(on); }
  void setLock() override { activeController()->setLock(); }
} g_router;

static ResistanceTest g_test(&g_router);

// ---- Status routing --------------------------------------------------------
static void handleStatus(const el15::Status &s) {
  ui::onStatus(s);
  if (g_test.running()) g_test.onStatus(s);
}

// ---- Demo simulator pump ---------------------------------------------------
static uint32_t g_lastSimMs = 0;

static void startDemo() {
  // Tear down any live BLE link first so the two transports can't interleave
  // (same rule as the Android DeviceCore.startSimulator fix).
  if (g_ble.state() != El15Client::IDLE) g_ble.shutdownAndDisconnect();
  g_demo = true;
  g_lastSimMs = millis();
  ui::onConnState(3 /* CONNECTED */, "Demo simulator");
}

static void stopAll() {
  if (g_test.running()) g_test.stop();
  if (g_demo) {
    g_demo = false;
    ui::onConnState(0 /* IDLE */, "Disconnected");
  } else {
    if (g_test.running()) g_ble.shutdownAndDisconnect();
    else g_ble.disconnect();
  }
}

void setup() {
  Serial.begin(115200);
  display::begin();

  UiActions actions;
  actions.scan       = []() { ui::clearDevices(); g_ble.startScan(8); };
  actions.connect    = [](const char *addr) { g_demo = false; g_ble.connectTo(addr); };
  actions.startDemo  = startDemo;
  actions.disconnect = stopAll;
  actions.setMode    = [](int m) { activeController()->setMode(m); };
  actions.setSetpoint= [](float v) { activeController()->setSetpoint(v); };
  actions.setLoad    = [](bool on) { activeController()->setLoad(on); };
  actions.lock       = []() { activeController()->setLock(); };
  actions.startRTest = [](float fuse, int steps) {
    g_test.steps = steps;
    g_test.start(fuse);
  };
  actions.stopRTest  = []() { g_test.stop(); };
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

  if (g_demo) {
    uint32_t now = millis();
    if (now - g_lastSimMs >= g_ble.pollIntervalMs) {
      uint32_t dt = now - g_lastSimMs;
      g_lastSimMs = now;
      handleStatus(g_sim.tick(dt));
    }
  }
  delay(2);
}
