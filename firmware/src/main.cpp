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

#include "audio.h"
#include "capacity_test.h"
#include "display.h"
#include "el15_client.h"
#include "el15_controller.h"
#include "link_guard.h"
#include "netclock.h"
#include "prefs.h"
#include "report.h"
#include "resistance_test.h"
#include "sd_card.h"
#include "ui.h"

static El15Client g_ble;

// The test engines drive the load through the BLE client (the only controller
// on this device).
static ResistanceTest g_test(&g_ble);
static CapacityTest g_batt(&g_ble);

// Watches the link whenever the load is energised — see link_guard.h.
static LinkGuard g_guard(&g_ble);

// Last completed result of each test, kept so the UI's "Save to SD card" can
// write a report whenever the user asks — the engines themselves keep nothing
// once they have handed the result over.
static ResistanceTest::Result g_lastRTest;
static CapacityTest::Result g_lastBatt;

// ---- Status routing --------------------------------------------------------
static void handleStatus(const el15::Status &s) {
  ui::onStatus(s);
  if (g_test.running()) g_test.onStatus(s);
  if (g_batt.running()) g_batt.onStatus(s);

  // Arm/disarm the link supervisor from the load's own reported state — the
  // device is the authority on whether current is flowing, not our intentions.
  if (s.valid) {
    if (s.loadOn)
      g_guard.arm(g_batt.running() ? prefs::Data::CAPACITY
                  : g_test.running() ? prefs::Data::RTEST
                                     : prefs::Data::MANUAL);
    else if (!g_test.running() && !g_batt.running())
      g_guard.disarm();
  }
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
  audio::fault();
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
  // A disconnect the user asked for is not a link loss: disarm AFTER the
  // LOAD_OFF above, so the guard never chases a link we dropped on purpose.
  g_guard.disarm();
}

void setup() {
  Serial.begin(115200);
  prefs::begin();   // before display/audio: they start from the stored values
  display::begin();
  display::setBrightness(prefs::get().brightness);
  display::setPixelShift(prefs::get().pixelShift);
  display::setIdleDim(prefs::get().idleDimS);
  audio::begin();
  audio::setVolume(prefs::get().volume);
  audio::setMuted(prefs::get().muted);
  audio::press();   // startup chime — confirms the codec is alive

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
  // A manual scan or connect is the user taking control — the guard stands down
  // so its recovery reconnects can't stop-scan on top of them.
  actions.scan       = []() { g_guard.standDown(); ui::clearDevices(); g_ble.startScan(8); };
  actions.connect    = [](const char *addr) { g_guard.standDown(); g_ble.connectTo(addr); };
  actions.disconnect = stopAll;
  actions.setMode    = [](int m) { g_ble.setMode(m); };
  actions.setSetpoint= [](float v) { g_ble.setSetpoint(v); };
  actions.setLoad    = [](bool on) {
    // Arm before the command, not after the status that confirms it: the link
    // can drop in between, and that window is exactly what the guard is for.
    if (on) g_guard.arm(prefs::Data::MANUAL);
    g_ble.setLoad(on);
  };
  actions.lock       = []() { g_ble.setLock(); };
  actions.setPollRate = [](int ms) {
    g_ble.pollIntervalMs = (uint32_t)ms;   // BLE status poll cadence
    g_test.pollIntervalMs = (uint32_t)ms;  // R-test settle/collect floors adapt
  };
  actions.startRTest = [](float fuse, int steps, bool fourWire, float tareOhm) {
    if (g_batt.running()) return;   // never let two engines drive the load
    g_guard.arm(prefs::Data::RTEST);
    g_test.steps = steps;
    g_test.fourWire = fourWire;
    g_test.tareOhm = tareOhm;
    g_test.start(fuse);
  };
  actions.stopRTest  = []() { g_test.stop(); };
  // Blocking (~1 s: card init dominates) and honest — the UI reports whatever
  // comes back in `msg`, file name or failure reason.
  actions.saveRTest  = [](char *msg, size_t len) {
    bool ok = report::saveRTest(g_lastRTest, msg, len);
    ok ? audio::success() : audio::failure();
    return ok;
  };
  actions.startBatt  = [](float cutoffV, float amps) {
    if (g_test.running()) return;   // never let two engines drive the load
    g_guard.arm(prefs::Data::CAPACITY);
    g_batt.cutoffV = cutoffV;
    g_batt.dischargeA = amps;
    g_batt.start();
  };
  actions.stopBatt   = []() { g_batt.stop(); };
  actions.saveBatt   = [](char *msg, size_t len) {
    bool ok = report::saveBatt(g_lastBatt, msg, len);
    ok ? audio::success() : audio::failure();
    return ok;
  };
  actions.sdInfo     = [](char *msg, size_t len) { return sd::info(msg, len); };
  actions.syncClock  = []() {
    const prefs::Data &p = prefs::get();
    net::start(p.ssid, p.pass, p.tzMinutes);
  };
  actions.scanWifi   = []() { return net::startScan(); };
  ui::begin(actions);

  net::onProgress = [](net::State st, const char *text) { ui::onNetProgress((int)st, text); };
  net::onScanDone = [](int n, const char *err) {
    // Marshal the scan results (owned by netclock until the next scan) into
    // plain arrays for the UI, which copies them.
    static const char *ssids[24];
    static int rssi[24];
    if (n > 24) n = 24;
    for (int i = 0; i < n; i++) { ssids[i] = net::scanSsid(i); rssi[i] = net::scanRssi(i); }
    ui::onWifiScanResult(ssids, rssi, n, err);
  };

  g_guard.onAlert = [](const char *title, const char *msg, bool resolved) {
    ui::onGuardAlert(title, msg, resolved);
  };
  g_guard.onAlarm = []() { audio::fault(); };

  g_ble.onState = [](El15Client::State st, const char *info) {
    if (st == El15Client::CONNECTED) {
      // Remember the peer so a link loss (or a crash) can get back to it
      // without a scan. Committed immediately: its whole value is surviving an
      // event we can't predict.
      prefs::change([](prefs::Data &d) {
        snprintf(d.lastAddr, sizeof(d.lastAddr), "%s", g_ble.lastAddress());
        d.lastAddrType = (uint8_t)g_ble.lastAddressType();
      });
      prefs::flush();
    } else {  // safe-off on drop
      if (g_test.running()) g_test.stop();
      if (g_batt.running()) g_batt.stop("Connection lost");
    }
    g_guard.onConnState(st);   // may start reconnect-and-force-off
    ui::onConnState((int)st, info);
  };
  g_ble.onStatus = handleStatus;
  g_ble.onDeviceFound = [](const char *addr, const char *name) { ui::onDeviceFound(addr, name); };
  g_ble.begin();

  g_test.onProgress = [](int s, int t, float tgt, float v, float i) { ui::onTestProgress(s, t, tgt, v, i); };
  g_test.onComplete = [](const ResistanceTest::Result &r) {
    g_lastRTest = r;   // keep it for a later Save to SD
    audio::success();
    ui::onTestComplete(r);
  };
  g_test.onError    = [](const char *m) { audio::failure(); ui::onTestError(m); };

  g_batt.onProgress = [](float v, float i, float ah, float wh, float temp, uint32_t el, int ph) {
    ui::onBattProgress(v, i, ah, wh, temp, el, ph);
  };
  g_batt.onComplete = [](const CapacityTest::Result &r) {
    g_lastBatt = r;   // keep it for a later Save to SD
    Serial.printf("[batt] done: %.3f Ah, %.1f Wh in %lus (%s)\n",
                  r.capacityAh, r.energyWh, (unsigned long)r.durationS, r.stopReason);
    audio::success();
    ui::onBattComplete(r);
  };
  g_batt.onError = [](const char *m) {
    Serial.printf("[batt] error: %s\n", m);
    audio::failure();
    ui::onBattError(m);
  };

  // ---- Crash / reboot recovery ---------------------------------------------
  // The in-flight flag is written synchronously whenever the load is energised
  // and cleared when it goes off, so finding it set here means the previous
  // session ended (panic, brownout, unplug) with current still flowing. Offer
  // to reconnect and force LOAD OFF rather than doing it silently: the user may
  // have already pulled the leads, and a surprise BLE connect is worse than a
  // prompt they can act on.
  uint8_t inFlight = prefs::get().inFlight;
  if (inFlight != prefs::Data::NONE) {
    const char *what = inFlight == prefs::Data::CAPACITY ? "a capacity test"
                     : inFlight == prefs::Data::RTEST    ? "a resistance test"
                                                         : "manual control";
    char msg[160];
    if (prefs::get().lastAddr[0])
      snprintf(msg, sizeof(msg),
               "The controller restarted (%s) during %s with the load ON. Tap to reconnect and force LOAD OFF.",
               rrs, what);
    else
      snprintf(msg, sizeof(msg),
               "The controller restarted (%s) during %s with the load ON, and no device is stored. Check the load by hand.",
               rrs, what);
    Serial.printf("[boot] recovery: in-flight=%u, peer=%s\n", (unsigned)inFlight,
                  prefs::get().lastAddr[0] ? prefs::get().lastAddr : "-");
    audio::fault();
    ui::offerRecovery(msg, []() { g_guard.start("Recovering after a restart"); });
  }
}

// ---- Physical buttons ------------------------------------------------------
static void handleButtons() {
  display::ButtonEvent ev = display::pollButtons();
  // Capture sleep state BEFORE noteActivity(): noteActivity() itself wakes the
  // screen (so any button counts as "the user is here" and resets the idle dim),
  // which would otherwise make the PWR toggle below read the already-woken state
  // and immediately sleep again — a press that woke then re-slept in one shot.
  bool wasAsleep = display::asleep();
  if (ev != display::BTN_NONE) display::noteActivity();
  switch (ev) {
    case display::BTN_BOOT_SHORT:
    case display::BTN_BOOT_LONG:
      // BOOT = hardware emergency stop. noteActivity() already woke the screen
      // so the user sees the acknowledgement; now kill the load.
      emergencyStop();
      break;
    case display::BTN_PWR_SHORT:
      audio::press();
      // Toggle against the PRE-wake state: a press while awake sleeps; a press
      // while asleep just wakes (noteActivity() did that already).
      if (!wasAsleep) display::setSleep(true);
      break;
    case display::BTN_PWR_LONG:
      // A held PWR key reads as "wake" only (noteActivity() handled it); leave
      // hardware power-off to the PMIC's own long-press handling.
      break;
    default: break;
  }
}

void loop() {
  // A running test must not have the screen blank out under it; dimming still
  // applies, which is what protects the panel during a multi-hour discharge.
  display::inhibitSleep(g_test.running() || g_batt.running());
  display::loopTick();
  handleButtons();
  g_ble.loopTick();
  g_test.tick();
  g_batt.tick();
  g_guard.tick();    // reconnect-and-force-off, if the link dropped hot
  net::tick();       // Wi-Fi NTP sync state machine (idle unless syncing)
  prefs::tick();     // debounced NVS commit
  delay(2);
}
