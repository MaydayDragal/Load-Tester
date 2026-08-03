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
#include "sample_log.h"
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

// One-shot: a clean boot with auto-connect enabled asks loop() to reconnect to
// the stored device once NimBLE has settled.
static bool g_autoConnectPending = false;

// Latched by a CONNECTED state, cleared by anything else. A test may only be
// torn down when the link actually DROPS — the raw "state != CONNECTED" test
// this replaced also fired on the user's own SCANNING/CONNECTING transitions,
// so opening Connect and tapping Scan mid-discharge silently killed the test.
static bool g_wasConnected = false;

// True while the capacity test is paused because OUR battery got critical, so
// loop() knows it may resume the test once power comes back.
static bool g_powerPaused = false;

// ---- Probe-wiring compensation (4-wire Kelvin / 2-wire lead tare) ----------
// The EL15 senses voltage at ITS OWN terminals, so in a 2-wire hook-up every
// reading is short by the drop across the test leads and their contacts:
//
//     V_at_the_device_under_test = V_at_the_load_terminals + I * R_leads
//
// At 3 A through 20 mohm that is 60 mV, and it is not a cosmetic error. It makes
// a battery cutoff fire early (so a capacity test stops before the pack is
// actually empty and under-reports it), it understates a source on the Monitor,
// and it grows with current — exactly when you care most.
//
// Once the lead resistance is known (measured by the R-Test tare sweep, or typed
// into Settings) the controller can put that drop back on every reading. In
// 4-wire (Kelvin) the sense path carries no current, so there is nothing to add:
// the reading already belongs to the device under test.
//
// Applied ONCE here, before the packet fans out, so the screen and the engines
// can never disagree about what the voltage is.
static el15::Status compensateProbe(const el15::Status &s) {
  const prefs::Data &p = prefs::get();
  if (!s.valid || p.fourWire || p.tareOhm <= 0) return s;
  el15::Status c = s;
  c.voltage = s.voltage + s.current * p.tareOhm;
  c.power = c.voltage * c.current;   // keep W consistent with the corrected V
  return c;
  // Deliberately NOT touched: `setpoint`. In CV mode that is a value the device
  // regulates at its own terminals — a command, not a measurement — so showing
  // it uncorrected next to a corrected reading is the honest pairing, and the
  // difference between them IS the lead drop.
}

// ---- Status routing --------------------------------------------------------
static void handleStatus(const el15::Status &s) {
  const el15::Status c = compensateProbe(s);
  ui::onStatus(c);
  // The R-test gets the RAW packet on purpose. Its fit already removes the lead
  // tare from the SLOPE (resistance_test.h correct()), and a pre-corrected
  // voltage would take the same resistance off a second time — the sweep would
  // report a DUT that is one tare too low, and a low-milliohm result could land
  // at zero. Every other consumer wants the corrected reading.
  if (g_test.running()) g_test.onStatus(s);
  if (g_batt.running()) g_batt.onStatus(c);

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

// Is anything (possibly) sinking current right now? An engine running, or the
// device reporting the load on (the guard arms on that echo).
static bool loadHot() {
  return g_test.running() || g_batt.running() || g_guard.armed();
}

// ---- Controller-brownout protection (AXP2101) ------------------------------
// The controller runs on its own battery; a multi-hour capacity test can outlast
// it. If the controller browns out mid-discharge it dies exactly like a crash —
// and the EL15 keeps sinking with nothing commanding it (the whole reason the
// link guard exists). So watch our OWN battery and, before we brown out, force
// LOAD OFF while a link still exists. Poll at 1 Hz from loop().
//
// Firmware-side threshold on the fuel-gauge percent (+ a VBAT hard floor), NOT
// the AXP2101's warning-IRQ register: that register's %-encoding is ambiguous in
// the vendor lib, whereas percent (reg 0xA4) is what the Settings card already
// reads reliably. On USB there is no brownout risk, so the check is suppressed.
static void monitorPower() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 1000) return;
  lastMs = millis();

  static const int CRIT_PCT = 8;        // fuel-gauge % — plenty of runway on 1S Li-ion
  static const int CRIT_MV = 3300;      // VBAT hard floor; below this the rails sag
  static const int DEBOUNCE = 3;        // consecutive 1 Hz reads before acting
  static int critCount = 0;
  static bool latched = false;

  int pct, mV, chg;
  bool present;
  if (!display::batteryStats(pct, mV, chg, present)) return;
  bool usb = display::usbPresent();

  // Feed the status-strip indicator from this poll. It has to happen BEFORE the
  // not-critical early return below, which is the path taken almost every
  // second — otherwise the indicator would only ever update while the battery
  // was already critical.
  ui::onControllerBattery(pct, mV, chg, present, usb);

  // One-shot at the first successful read: says whether a pack is even fitted,
  // which is the difference between "the indicator is broken" and "the indicator
  // is correctly showing nothing".
  static bool loggedBatt = false;
  if (!loggedBatt) {
    loggedBatt = true;
    Serial.printf("[pmic] controller battery: %s, %d%%, %d mV, charge=%d, usb=%d\n",
                  present ? "present" : "ABSENT (running on USB)", pct, mV, chg, (int)usb);
  }

  bool crit = present && !usb &&
              (pct <= CRIT_PCT || (mV > 2500 && mV <= CRIT_MV));  // mV range guards a garbled read
  if (!crit) {
    critCount = 0;
    latched = false;
    // Power is back (USB plugged in, or the pack recovered). A capacity test we
    // paused for this reason can pick up exactly where it left off — hours of
    // discharge data were never thrown away, so finishing the run is just a
    // matter of re-energising the load.
    if (g_powerPaused) {
      g_powerPaused = false;
      if (g_batt.paused() && g_batt.resume()) {
        Serial.println("[pmic] controller power restored - capacity test resumed");
        audio::success();
      }
    }
    return;
  }
  if (++critCount < DEBOUNCE || latched) return;
  latched = true;   // act once per sustained low-battery episode

  bool wasHot = loadHot();
  Serial.printf("[pmic] controller battery CRITICAL (%d%%, %d mV) - forcing LOAD OFF (wasHot=%d)\n",
                pct, mV, (int)wasHot);
  // A sweep cannot survive an interruption (its V-I ladder would have a hole in
  // it), so it stops. A capacity discharge CAN: pause it, keep the accumulated
  // Ah/Wh and the flash datapoint log, and resume when power returns.
  if (g_test.running()) g_test.stop();
  if (g_batt.running()) {
    if (g_batt.pause("Controller battery critical")) g_powerPaused = true;
    else g_batt.stop("Controller battery critical");
  }
  g_ble.setSetpoint(0);
  g_ble.setLoad(false);   // the guard's in-flight NVS flag (set while armed) covers a hard brownout
  audio::fault();
  ui::onPowerCritical(pct, wasHot);
}

// ---- Load-safe power-off (long-press PWR) ----------------------------------
// A power-off must never strand the load: force LOAD OFF and let the write flush
// over BLE BEFORE cutting our own power via the AXP2101. The PMIC's own OFFLEVEL
// hardware power-off would skip this, so we intercept the long-press IRQ (which
// fires earlier) and do a deterministic clean shutdown instead.
static void powerOffSafely() {
  if (g_test.running()) g_test.stop();
  if (g_batt.running()) g_batt.stop("Powering off");
  g_ble.setSetpoint(0);
  g_ble.setLoad(false);
  Serial.println("[btn] POWER OFF (load stopped)");
  ui::onPoweringOff();
  audio::press();
  delay(250);   // let the LOAD_OFF notification transmit before power is cut
  // If a live link confirmed the off, this is a clean shutdown — clear the
  // recovery flag. If we could NOT reach the load (disconnected while armed),
  // leave the in-flight flag set so the next boot offers reconnect-and-kill.
  if (g_ble.state() == El15Client::CONNECTED) prefs::clearInFlight();
  prefs::flush();
  display::powerOff();   // AXP2101: cut all rails
  delay(600);            // powerOff() should not return; fall back to a reset
  ESP.restart();
}

static void stopAll() {
  // Capture whether a test was mid-run BEFORE stopping it: stopping the test
  // clears the flag, so checking it afterwards would always be false and the
  // safe LOAD_OFF-then-flush teardown would never run (mirrors the Android
  // DeviceCore.disconnect() `wasBusy` capture).
  // "Hot" = anything may be sinking current: a running engine OR a manually
  // energised load (the guard arms whenever the device reports loadOn). A
  // plain disconnect while hot would leave the EL15 sinking with no controller
  // attached and no supervisor — Disconnect must never be a way to walk away
  // from flowing current.
  bool wasHot = g_test.running() || g_batt.running() || g_guard.armed();
  if (g_test.running()) g_test.stop();
  if (g_batt.running()) g_batt.stop("Disconnected");
  if (wasHot) g_ble.shutdownAndDisconnect();  // push LOAD_OFF and let it flush
  else g_ble.disconnect();
  // A disconnect the user asked for is not a link loss: disarm AFTER the
  // LOAD_OFF above, so the guard never chases a link we dropped on purpose.
  g_guard.disarm();
}

#ifdef EL15_RTUNE
// Defined below (after the button handlers) but referenced from setup()'s
// engine callbacks.
namespace rtune {
void onDone(const ResistanceTest::Result &r);
void onFail(const char *msg);
void begin();
void tick();
}  // namespace rtune
#endif

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
  // so its recovery reconnects can't stop-scan on top of them, and its stale
  // banner (locked warning / retry offer) is dismissed with it.
  actions.scan       = []() { g_guard.standDown(); ui::clearGuardBanner(); ui::clearDevices(); g_ble.startScan(8); };
  actions.connect    = [](const char *addr) { g_guard.standDown(); ui::clearGuardBanner(); g_ble.connectTo(addr); };
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
  actions.startRTest = [](float fuse, float startA, float maxA, uint32_t sweepS,
                          bool fourWire, float tareOhm) {
    if (g_batt.running()) return;   // never let two engines drive the load
    g_guard.arm(prefs::Data::RTEST);
    g_test.startCurrent = startA;
    g_test.maxCurrent = maxA;       // 0 = use the fuse's full safe headroom
    g_test.sweepSeconds = sweepS;
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
  actions.startBatt  = [](float cutoffV, float amps, float ratedAh, int chem, int cells) {
    if (g_test.running()) return;   // never let two engines drive the load
    g_guard.arm(prefs::Data::CAPACITY);
    g_batt.cutoffV = cutoffV;
    g_batt.dischargeA = amps;
    g_batt.ratedAh = ratedAh;   // 0 = not given; SoH is then suppressed
    g_batt.chemistry = chem;    // drives the discharge-curve time estimate
    g_batt.cells = cells;
    // Snapshot the probe wiring for the report. Read at START, not at save
    // time: a Settings change between the run and a (re)save must not relabel
    // data that was recorded under the old setting.
    g_batt.fourWire = prefs::get().fourWire;
    g_batt.tareOhm = prefs::get().tareOhm;
    g_batt.start();
  };
  actions.stopBatt   = []() { g_batt.stop(); };
  actions.resumeBatt = []() {
    // Re-arm before the load goes back on, same ordering rule as the manual
    // load button: the link can drop in the window between the two.
    if (!g_batt.paused()) return false;
    g_guard.arm(prefs::Data::CAPACITY);
    g_powerPaused = false;   // a manual resume takes the auto-resume off the table
    return g_batt.resume();
  };
  actions.battRemainingS = []() { return g_batt.remainingS(); };
  actions.battSocPct = []() { return g_batt.socPct(); };
  actions.battEtaFromCurve = []() { return g_batt.etaFromCurve(); };
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

  // RAM is the binding constraint on this board — no PSRAM, and LVGL allocates
  // from the system heap. NimBLE needs a ~30 KB CONTIGUOUS block to ESTABLISH a
  // connection, and running short of it presents as "Connect failed" (HCI 0x3e)
  // rather than as an out-of-memory error, which is exactly the trap this cost a
  // session to before (HANDOVER §7). So print both numbers once the UI has
  // finished allocating: the margin is then observable on any boot instead of
  // being rediscovered the next time a screen grows.
  Serial.printf("[boot] heap after UI: %u B free, largest block %u B (BLE connect needs ~30 KB contiguous)\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

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
  // Final give-up: an ACTIONABLE banner (tap = retry) instead of a locked
  // dead-end — a permanently locked banner also suppressed later protection
  // alerts for the rest of the session.
  g_guard.onFailed = [](const char *msg) {
    ui::offerRecovery(msg, []() { g_guard.retry(); });
  };
  g_guard.onAlarm = []() { audio::fault(); };

  g_ble.onState = [](El15Client::State st, const char *info) {
    if (st == El15Client::CONNECTED) {
      g_wasConnected = true;
      // Remember the peer so a link loss (or a crash) can get back to it
      // without a scan. Committed immediately: its whole value is surviving an
      // event we can't predict.
      prefs::change([](prefs::Data &d) {
        snprintf(d.lastAddr, sizeof(d.lastAddr), "%s", g_ble.lastAddress());
        d.lastAddrType = (uint8_t)g_ble.lastAddressType();
      });
      prefs::flush();
    } else {
      // Safe-off, but ONLY on a genuine drop from a live link. SCANNING and
      // CONNECTING are also "not CONNECTED", and treating them as a drop meant
      // that opening Connect and tapping Scan during a discharge tore the test
      // down — mid-priming it even died with "Cancelled" and left nothing to
      // save. The guard keeps its own copy of this latch for its own recovery.
      bool dropped = g_wasConnected;
      g_wasConnected = false;
      if (dropped) {
        if (g_test.running()) g_test.stop();
        // A discharge survives a link loss as a PAUSE: the load is off either
        // way, but the hours of Ah/Wh and the flash datapoint log are kept and
        // the user can resume once the link is back.
        if (g_batt.running() && !g_batt.pause("BLE link lost")) g_batt.stop("Connection lost");
      }
    }
    g_guard.onConnState(st);   // may start reconnect-and-force-off
    ui::onConnState((int)st, info);
  };
  g_ble.onStatus = handleStatus;
  g_ble.onDeviceFound = [](const char *addr, const char *name) { ui::onDeviceFound(addr, name); };
  g_ble.begin();

  g_test.onProgress = [](float el, float total, float tgt, float v, float i, float r, bool rOk) {
    ui::onTestProgress(el, total, tgt, v, i, r, rOk);
  };
  g_test.onComplete = [](const ResistanceTest::Result &r) {
    g_lastRTest = r;   // keep it for a later Save to SD
#ifdef EL15_RTUNE
    // The tuning harness owns the engine: report to serial and go straight to
    // the next run, without the UI's auto-save writing a card file per sweep.
    rtune::onDone(r);
    return;
#endif
    audio::success();
    ui::onTestComplete(r);
  };
  g_test.onError    = [](const char *m) {
#ifdef EL15_RTUNE
    rtune::onFail(m);
    return;
#endif
    audio::failure(); ui::onTestError(m);
  };

  g_batt.onProgress = [](float v, float i, float ah, float wh, float temp, uint32_t el, int ph) {
    ui::onBattProgress(v, i, ah, wh, temp, el, ph);
  };
  g_batt.onComplete = [](const CapacityTest::Result &r) {
    g_lastBatt = r;   // keep it for a later Save to SD
    Serial.printf("[batt] done: %.3f Ah, %.1f Wh in %lus (%s)\n",
                  r.capacityAh, r.energyWh, (unsigned long)r.durationS, r.stopReason);
    audio::success();
    // ui::onBattComplete paints the result and then drives the automatic save
    // itself, so the "Saving..." state is on screen before the card blocks the
    // loop task. The flash datapoint log stays intact until the next start(),
    // so a failed auto-save can still be retried from the button.
    ui::onBattComplete(r);
  };
  g_batt.onPause = [](bool paused, const char *reason) { ui::onBattPaused(paused, reason); };
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
  } else if (prefs::get().autoConnect && prefs::get().lastAddr[0]) {
    // Clean boot + auto-connect enabled + a device on record: reconnect on our
    // own so the user doesn't scan+tap every session. Deferred to loop() so
    // NimBLE's host has time to sync first; skipped when crash recovery owns the
    // boot (its explicit reconnect-and-force-off takes precedence above).
    g_autoConnectPending = true;
  }

#ifdef EL15_SDTEST
  // On-device SD card exercise. Runs the SAME sd::info()/sd::saveCsv() paths the
  // UI buttons use, on the loop task. Reports card presence/size, writes two real
  // reports (checks the NNN auto-increment), and reads one back to prove the
  // bytes actually landed.
  delay(300);   // let the panel finish its bring-up before the blocking card init
  Serial.println("[sdtest] ===== SD CARD TEST =====");
  {
    int Y, M, D, h, mi, s;
    if (display::rtcTime(Y, M, D, h, mi, s))
      Serial.printf("[sdtest] RTC reads %04d-%02d-%02d %02d:%02d:%02d\n", Y, M, D, h, mi, s);
    else
      Serial.println("[sdtest] RTC NOT SET - reports will stamp uptime, files get a zero date");
  }
  char sdmsg[80];
  bool okInfo = sd::info(sdmsg, sizeof(sdmsg));
  Serial.printf("[sdtest] info : %-4s -> %s\n", okInfo ? "OK" : "FAIL", sdmsg);
  if (okInfo) {
    auto body = [](Print &f) {
      report::fpf(f, "# EL15 SD self-test\n");
      report::writeStamp(f);
      report::fpf(f, "n,square\n");
      for (int i = 1; i <= 5; i++) report::fpf(f, "%d,%d\n", i, i * i);
      return f.getWriteError() == 0;
    };
    bool w1 = sd::saveCsv("SDTEST", body, sdmsg, sizeof(sdmsg));
    Serial.printf("[sdtest] write1: %-4s -> %s\n", w1 ? "OK" : "FAIL", sdmsg);
    char name1[sizeof(sdmsg)]; snprintf(name1, sizeof(name1), "%s", sdmsg);
    bool w2 = sd::saveCsv("SDTEST", body, sdmsg, sizeof(sdmsg));
    Serial.printf("[sdtest] write2: %-4s -> %s (expect the index to increment)\n",
                  w2 ? "OK" : "FAIL", sdmsg);
    if (w1) {
      Serial.printf("[sdtest] readback of %s:\n", name1);
      bool rb = sd::readBackTest(name1, sdmsg, sizeof(sdmsg));
      Serial.printf("[sdtest] readback: %-4s -> %s\n", rb ? "OK" : "FAIL", sdmsg);
    }
    bool okInfo2 = sd::info(sdmsg, sizeof(sdmsg));
    Serial.printf("[sdtest] info2: %-4s -> %s (card still healthy after the writes)\n",
                  okInfo2 ? "OK" : "FAIL", sdmsg);
  }

  // ---- Flash datapoint log (sample_log.h) ---------------------------------
  // Exercises the whole path the capacity test uses — mount, tiered append,
  // batch flush, replay — without energising anything.
  Serial.printf("[sdtest] free heap before LittleFS: %u B\n", (unsigned)ESP.getFreeHeap());
  if (samplelog::start()) {
    Serial.printf("[sdtest] free heap after  LittleFS: %u B\n", (unsigned)ESP.getFreeHeap());
    // 600 offered samples one simulated second apart. At a 2 s base interval
    // that must store ~300 of them, proving the tier schedule rate-limits.
    for (uint32_t t = 0; t < 600; t++)
      samplelog::add(t, 12.0f - t * 0.001f, 1.5f, 30.0f + t * 0.01f,
                     1.5f * t / 3600.0f, 18.0f * t / 3600.0f);
    samplelog::flush();
    Serial.printf("[sdtest] log stored %lu records at %lu s interval (expect ~300 @ 2 s)\n",
                  (unsigned long)samplelog::count(), (unsigned long)samplelog::intervalS());
    uint32_t seen = 0, firstT = 0, lastT = 0;
    float firstV = 0, lastV = 0;
    samplelog::replay([&](const samplelog::Rec &r) {
      if (seen == 0) { firstT = r.tS; firstV = r.v; }
      lastT = r.tS; lastV = r.v; seen++;
      return true;
    });
    Serial.printf("[sdtest] replay read %lu records, t %lu..%lu s, V %.3f..%.3f\n",
                  (unsigned long)seen, (unsigned long)firstT, (unsigned long)lastT,
                  firstV, lastV);
    Serial.printf("[sdtest] log %s\n",
                  (seen == samplelog::count() && seen > 0) ? "OK" : "FAIL (count mismatch)");
  } else {
    Serial.println("[sdtest] log FAIL - LittleFS unavailable");
  }
  Serial.println("[sdtest] ===== END =====");
#endif
}

#ifdef EL15_RTUNE
// ---- R-test tuning harness (build-flag gated, not in normal builds) --------
// Runs a matrix of sweeps back-to-back and prints one CSV row per run, so the
// sweep parameters can be chosen from measured data instead of guesswork.
//
// The number that matters is NOT the sigma each fit reports about itself — it is
// the run-to-run SPREAD of R across repeats of the same configuration. A fit can
// be confidently wrong; only repetition shows that. Every configuration is
// therefore run REPEATS times and the summary compares the two.
//
// Safety: the peak current here is deliberately far below what the fuse/ratings
// would allow. Accuracy tuning is about repeatability, not amplitude, and the
// source on the bench is not necessarily something that wants to be loaded hard.
namespace rtune {

struct Cfg {
  uint32_t sweepS;
  uint32_t pollMs;
  float maxA;
  uint32_t setpointMs;   // 0 = engine default (derived from the poll interval)
  uint32_t settleMs;     // 0 = keep every sample
  uint32_t gapMs;        // control-write -> poll separation
  const char *name;
};

static const Cfg MATRIX[] = {
#if defined(EL15_RTUNE_STAGE3)
    // Ramp quality: how often the setpoint is re-commanded, and whether
    // discarding the load's switch-on transient helps.
    {10, 50, RTUNE_PEAK_A, 100, 0, 25, "gap25"},
    {10, 50, RTUNE_PEAK_A, 100, 0, 40, "gap40"},
    {10, 50, RTUNE_PEAK_A, 100, 0, 60, "gap60"},
    
#elif defined(EL15_RTUNE_STAGE2)
    {10,  50, RTUNE_PEAK_A, 0, 0, 40, "10s@20Hz"},
    {20,  50, RTUNE_PEAK_A, 0, 0, 40, "20s@20Hz"},
    {30,  50, RTUNE_PEAK_A, 0, 0, 40, "30s@20Hz"},
    {60,  50, RTUNE_PEAK_A, 0, 0, 40, "60s@20Hz"},
    {30, 100, RTUNE_PEAK_A, 0, 0, 40, "30s@10Hz"},
    {30, 250, RTUNE_PEAK_A, 0, 0, 40, "30s@4Hz"},
#else
    // Stage 1: one short, gentle probe to characterise the source before
    // deciding whether a fuller matrix is safe.
    {10, 50, RTUNE_PEAK_A, 0, 0, 40, "probe"},
#endif
};
static const int N_CFG = (int)(sizeof(MATRIX) / sizeof(MATRIX[0]));
#ifdef EL15_RTUNE_STAGE2
static const int REPEATS = 3;
#else
static const int REPEATS = 2;
#endif

static int cfgIdx = 0, rep = 0;
static bool active = false, waiting = false, done = false;
static uint32_t nextAtMs = 0;
static float rOf[N_CFG][8];      // measured R per repeat, for the spread summary
static int rN[N_CFG];

void summarise() {
  Serial.println("[rtune] ===== SUMMARY (run-to-run spread is the real accuracy) =====");
  Serial.println("[rtune] config,runs,mean_mohm,spread_mohm,spread_pct");
  for (int c = 0; c < N_CFG; c++) {
    if (rN[c] < 2) continue;
    double sum = 0;
    for (int k = 0; k < rN[c]; k++) sum += rOf[c][k];
    double mean = sum / rN[c];
    double var = 0;
    for (int k = 0; k < rN[c]; k++) { double d = rOf[c][k] - mean; var += d * d; }
    double sd = sqrt(var / (rN[c] - 1));
    Serial.printf("[rtune] %s,%d,%.3f,%.3f,%.2f\n", MATRIX[c].name, rN[c],
                  mean * 1000.0, sd * 1000.0,
                  mean > 1e-9 ? sd / mean * 100.0 : 0.0);
  }
  Serial.println("[rtune] ===== END =====");
}

void startNext() {
  const Cfg &c = MATRIX[cfgIdx];
  g_ble.pollIntervalMs = c.pollMs;
  g_test.pollIntervalMs = c.pollMs;
  g_test.startCurrent = 0;
  g_test.maxCurrent = c.maxA;
  g_test.sweepSeconds = c.sweepS;
  g_test.setpointMs = c.setpointMs;
  g_test.settleMs = c.settleMs;
  g_ble.ctrlPollGapMs = c.gapMs;
  g_test.fourWire = false;
  g_test.tareOhm = 0;
  Serial.printf("[rtune] run %s rep %d/%d: %lu s ramp to %.2f A, poll %lu ms, setpoint %lu ms, settle %lu ms\n",
                c.name, rep + 1, REPEATS, (unsigned long)c.sweepS, c.maxA,
                (unsigned long)c.pollMs, (unsigned long)c.setpointMs, (unsigned long)c.settleMs);
  Serial.printf("[rtune]   ctrl->poll gap %lu ms\n", (unsigned long)c.gapMs);
  waiting = true;
  g_guard.arm(prefs::Data::RTEST);
  g_test.start(30.0f);   // fuse rating high enough not to bind; maxCurrent rules
}

void onDone(const ResistanceTest::Result &r) {
  if (!active) return;
  const Cfg &c = MATRIX[cfgIdx];
  Serial.printf("[rtune] RESULT,%s,%d,%.6f,%.6f,%.5f,%d,%.4f,%.4f,%.4f,%.3f,%.2f,%d,%s\n",
                c.name, rep + 1, r.resistanceOhm, r.resistanceStdErr, r.rSquared,
                r.rawSamples, r.openCircuitVoltage, r.minCurrent, r.maxCurrentSeen,
                r.sagV, r.tempMax - r.tempMin, r.loadDropouts,
                r.reliable ? "yes" : "no");
  if (rN[cfgIdx] < 8) rOf[cfgIdx][rN[cfgIdx]++] = r.resistanceOhm;
  waiting = false;
  // Bail out if the source turns out not to be stiff enough for the current we
  // are asking of it. A sag past 10 % of the open-circuit voltage means we are
  // loading it near its limit, which is neither a good measurement nor a polite
  // thing to keep doing to somebody's bench supply unattended.
  if (r.openCircuitVoltage > 1.0f && r.sagV > 0.10f * r.openCircuitVoltage) {
    Serial.printf("[rtune] STOPPING: %.2f V sag on a %.2f V source is >10%% - "
                  "the source is not stiff enough for %.2f A\n",
                  r.sagV, r.openCircuitVoltage, MATRIX[cfgIdx].maxA);
    active = false; done = true;
    summarise();
    return;
  }
  // Let the wiring settle back to ambient between runs, so a later run does not
  // inherit the previous one's self-heating.
  nextAtMs = millis() + 4000;
  if (++rep >= REPEATS) { rep = 0; cfgIdx++; }
  if (cfgIdx >= N_CFG) { active = false; done = true; summarise(); }
}

void onFail(const char *msg) {
  Serial.printf("[rtune] ABORTED: %s\n", msg);
  active = false; waiting = false; done = true;
  summarise();
}

void begin() {
  Serial.println("[rtune] ===== R-TEST TUNING MATRIX =====");
  Serial.println("[rtune] RESULT,config,rep,R_ohm,sigma_ohm,r2,samples,voc,imin,imax,sag,dTemp,dropouts,reliable");
  for (int c = 0; c < N_CFG; c++) rN[c] = 0;
  cfgIdx = 0; rep = 0; active = true; waiting = false; done = false;
  nextAtMs = millis() + 1500;
}

void tick() {
  if (!active || waiting) return;
  if ((int32_t)(millis() - nextAtMs) < 0) return;
  startNext();
}

}  // namespace rtune
#endif  // EL15_RTUNE

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
      // Asleep: a blind long-press just wakes (don't power off something the
      // user can't see). Awake: a deliberate long hold powers the controller
      // off — but LOAD-SAFE (force LOAD OFF first), unlike the PMIC's own
      // OFFLEVEL cutoff which would strand an energised load.
      if (wasAsleep) break;   // noteActivity() already woke it
      powerOffSafely();
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
  // Startup auto-connect (once, after NimBLE has had ~1.5 s to sync). connectTo
  // blocks the loop, but nothing else needs to run yet; the UI paints
  // "Connecting..." via onConnState.
  if (g_autoConnectPending && millis() > 1500) {
    g_autoConnectPending = false;
    if (g_ble.state() == El15Client::IDLE && prefs::get().lastAddr[0]) {
      Serial.printf("[ble] auto-connect to %s (type %u)\n",
                    prefs::get().lastAddr, (unsigned)prefs::get().lastAddrType);
      g_ble.connectTo(prefs::get().lastAddr, prefs::get().lastAddrType);
#ifdef EL15_RTUNE
      // Fail-safe for the unattended test build: if the previous session ended
      // with the load energised (a reset mid-sweep), force it off the moment we
      // are back on the link rather than waiting for somebody to tap a banner.
      // The interactive firmware deliberately asks first; a headless test rig
      // has nobody to ask.
      if (g_ble.state() == El15Client::CONNECTED) {
        Serial.println("[rtune] boot: forcing LOAD OFF before anything else");
        for (int k = 0; k < 3; k++) { g_ble.setSetpoint(0); g_ble.setLoad(false); delay(120); }
        prefs::clearInFlight();
      }
#endif
    }
  }
#ifdef EL15_RTUNE
  {
    // NEVER auto-start. Opening a serial monitor resets this board (the
    // USB-Serial/JTAG peripheral treats the DTR/RTS transitions as a reset
    // request), and on 2026-08-01 that landed mid-sweep: the ESP rebooted with
    // the load at ~1 A, the EL15 was left sinking with nothing commanding it,
    // and it ran until its own protection tripped. So the matrix waits for a
    // key press on the already-open port — by which time every reset that
    // monitoring is going to cause has already happened, with the load off.
    static bool started = false, prompted = false;
    if (!started && g_ble.state() == El15Client::CONNECTED) {
      if (!prompted) {
        prompted = true;
        Serial.println("[rtune] ready - press any key on this port to start the matrix");
      }
      if (Serial.available()) {
        while (Serial.available()) Serial.read();
        started = true;
        rtune::begin();
      }
    }
    rtune::tick();
  }
#endif
  g_test.tick();
  g_batt.tick();
  g_guard.tick();    // reconnect-and-force-off, if the link dropped hot
  monitorPower();    // 1 Hz: force LOAD OFF before the CONTROLLER's own battery dies
  net::tick();       // Wi-Fi NTP sync state machine (idle unless syncing)
  prefs::tick();     // debounced NVS commit
  delay(2);
}
