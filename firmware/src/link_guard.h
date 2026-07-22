// Link-loss auto-stop supervisor + crash/reboot recovery.
//
// The gap this closes: the EL15 keeps sinking current after the BLE link drops.
// Nothing on this controller can stop it — every command goes over that link —
// so a dropped connection mid-discharge means a battery keeps draining until
// the load's own UVP catches it, or doesn't. The engines' stop() paths write
// LOAD_OFF into a dead link and silently achieve nothing.
//
// So: whenever the load is (or might be) energised, the guard is ARMED, which
// means two things.
//   1. A dropped link starts a reconnect-and-kill loop: reconnect, push
//      LOAD_OFF + setpoint 0 several times, and only then call it resolved.
//      Every attempt is announced on screen and audibly, and running out of
//      attempts is reported as a failure to act on — not quietly dropped.
//   2. The armed state is written to NVS *synchronously* (prefs::armInFlight),
//      so if the controller panics or browns out instead, the next boot knows
//      the load was left on and can offer the same reconnect-and-kill.
//
// Loop task only. tick() may block for the duration of one connect attempt
// (connect timeout is shortened to RECONNECT_TIMEOUT_MS for this reason), and
// the UI is told to paint the alert before that happens.
#pragma once

#include <Arduino.h>
#include <functional>

#include "el15_client.h"
#include "prefs.h"

class LinkGuard {
 public:
  // Progress/outcome for the UI. `resolved` distinguishes "the load is off,
  // stand down" from "still trying" / "gave up".
  std::function<void(const char *title, const char *msg, bool resolved)> onAlert;
  std::function<void()> onAlarm;   // audible warning, repeated per attempt

  explicit LinkGuard(El15Client *ble) : ble_(ble) {}

  // Arm/disarm from whatever knows the load's state (a status packet saying
  // loadOn, a test starting, the user pressing LOAD ON). Cheap to call
  // repeatedly: NVS only writes when the value actually changes.
  void arm(uint8_t what) {
    armed_ = what;
    prefs::armInFlight(what);
  }

  void disarm() {
    if (!armed_ && state_ == IDLE) return;
    armed_ = prefs::Data::NONE;
    prefs::clearInFlight();
    if (state_ != IDLE) state_ = IDLE;
  }

  bool armed() const { return armed_ != prefs::Data::NONE; }
  bool recovering() const { return state_ != IDLE; }

  // Called by main on every BLE state change.
  void onConnState(El15Client::State st) {
    if (st == El15Client::CONNECTED) return;
    // A disconnect while nothing is energised is just a disconnect.
    if (!armed() || state_ != IDLE) return;
    start(armed_ == prefs::Data::CAPACITY ? "Link lost during a discharge"
                                          : "Link lost with the load ON");
  }

  // Also the entry point for boot-time recovery, where there was no live link
  // to lose — the previous session simply never got to turn the load off.
  void start(const char *why) {
    if (state_ != IDLE) return;
    if (!haveTarget()) {   // nothing to reconnect to; say so rather than pretend
      if (onAlert) onAlert("LOAD MAY STILL BE ON", "No known device to reconnect to - disconnect the load manually.", false);
      return;
    }
    state_ = RECOVERING;
    attempts_ = 0;
    nextTryMs_ = millis();   // first attempt immediately
    Serial.printf("[guard] recovery started: %s\n", why);
    if (onAlert) onAlert("LOAD MAY STILL BE ON", why, false);
    if (onAlarm) onAlarm();
  }

  void tick() {
    if (state_ != RECOVERING) return;
    if ((int32_t)(millis() - nextTryMs_) < 0) return;
    attempts_++;

    char msg[96];
    snprintf(msg, sizeof(msg), "Reconnecting to force LOAD OFF - attempt %d of %d...",
             attempts_, MAX_ATTEMPTS);
    if (onAlert) onAlert("LOAD MAY STILL BE ON", msg, false);

    ble_->setConnectTimeoutMs(RECONNECT_TIMEOUT_MS);
    bool up = ble_->state() == El15Client::CONNECTED ||
              ble_->connectTo(target(), targetType());
    ble_->setConnectTimeoutMs(NORMAL_TIMEOUT_MS);

    if (up) {
      forceOff();
      state_ = IDLE;
      armed_ = prefs::Data::NONE;
      prefs::clearInFlight();
      Serial.println("[guard] recovery succeeded - LOAD OFF pushed");
      if (onAlert) onAlert("LOAD FORCED OFF", "Reconnected and shut the load down. Check the setup before restarting.", true);
      return;
    }

    if (attempts_ >= MAX_ATTEMPTS) {
      state_ = FAILED;
      Serial.println("[guard] recovery FAILED - could not reach the load");
      if (onAlert) onAlert("CANNOT REACH THE LOAD",
                           "The load did not answer. DISCONNECT IT BY HAND - it may still be drawing current.", false);
      if (onAlarm) onAlarm();
      return;
    }
    if (onAlarm) onAlarm();
    nextTryMs_ = millis() + RETRY_INTERVAL_MS;
  }

  // Retry a failed recovery (from a UI button).
  void retry() {
    if (state_ == FAILED) { state_ = IDLE; }
    start("Retrying");
  }

 private:
  enum State { IDLE, RECOVERING, FAILED };

  static const int MAX_ATTEMPTS = 8;
  static const uint32_t RETRY_INTERVAL_MS = 2500;
  // Short enough that the UI is only frozen for a few seconds per attempt.
  static const uint32_t RECONNECT_TIMEOUT_MS = 4000;
  static const uint32_t NORMAL_TIMEOUT_MS = 30000;   // NimBLE's default

  // Prefer this session's peer; fall back to the stored one after a reboot.
  bool haveTarget() const { return target()[0] != '\0'; }
  const char *target() const {
    return ble_->lastAddress()[0] ? ble_->lastAddress() : prefs::get().lastAddr;
  }
  int targetType() const {
    return ble_->lastAddress()[0] ? ble_->lastAddressType() : prefs::get().lastAddrType;
  }

  // Repeat the shutdown: a single write can be lost on a link that has only
  // just come back, and this is the one command that must land.
  void forceOff() {
    for (int i = 0; i < 3; i++) {
      ble_->setSetpoint(0);
      ble_->setLoad(false);
      delay(120);
    }
  }

  El15Client *ble_;
  State state_ = IDLE;
  uint8_t armed_ = prefs::Data::NONE;
  int attempts_ = 0;
  uint32_t nextTryMs_ = 0;
};
