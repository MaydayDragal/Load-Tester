// Battery capacity discharge test — see CAPACITY_PLAN.md.
//
// Runs the load in CC mode and integrates Ah/Wh locally (trapezoid over sample
// timestamps; CC status packets also carry temperature, which CAP-mode packets
// do not). Discharges until the debounced minimum-voltage cutoff, a safety cap
// (max time / max Ah), a protection trip, or a manual stop; then optionally
// rests with the load off to capture the voltage rebound before reporting.
//
// Same architecture and safety discipline as ResistanceTest: timer-driven
// state machine pumped by tick() from loop(), fed live readings via
// onStatus(), LOAD_OFF + setpoint 0 on every exit path. Every stop() fires
// either onComplete (any discharge data exists) or onError, so the UI never
// has to guess the engine's state.
#pragma once

#include <Arduino.h>
#include <functional>
#include "el15_controller.h"
#include "el15_protocol.h"

class CapacityTest {
 public:
  struct Result {
    float capacityAh = 0, energyWh = 0;
    uint32_t durationS = 0;
    float startV = 0, endV = 0, reboundV = 0;
    float avgV = 0, avgI = 0;
    float minTemp = 0, maxTemp = 0;
    int maxFan = 0;
    float cutoffV = 0, currentA = 0;
    const char *stopReason = "";   // static string
  };

  // Configuration — set before start().
  float cutoffV = 0;                     // automatic minimum-voltage stop point
  float dischargeA = 0;                  // requested discharge current
  uint32_t maxDurationS = 12u * 3600u;   // safety cap
  float maxAh = 50;                      // safety cap
  uint32_t restS = 60;                   // post-cutoff rebound window (0 = none)

  // Callbacks (fired on the loop task). phase: 1 = discharging, 2 = resting.
  std::function<void(float v, float i, float ah, float wh, float temp,
                     uint32_t elapsedS, int phase)> onProgress;
  std::function<void(const Result &)> onComplete;
  std::function<void(const char *)> onError;

  explicit CapacityTest(El15Controller *ctrl) : ble_(ctrl) {}

  bool running() const { return state_ != IDLE; }

  void start() {
    if (running()) return;
    if (cutoffV <= 0.05f || dischargeA <= 0.005f) {
      if (onError) onError("Set a cutoff voltage and discharge current first.");
      return;
    }
    ah_ = wh_ = sumVdt_ = sumDt_ = 0;
    vOc_ = vNow_ = startV_ = endV_ = reboundV_ = 0;
    minT_ = 1e9f; maxT_ = -1e9f; fanMax_ = 0;
    below_ = 0; lastMs_ = 0; haveSample_ = false;
    stopReason_ = "";
    state_ = PRIMING;
    ble_->setMode(el15::MODE_CC);
    ble_->setSetpoint(0);
    ble_->setLoad(false);
    timerAt_ = millis() + 1500;   // a couple of samples of open-circuit voltage
    timerCb_ = PRIME_DONE;
  }

  // Manual/external stop. Mid-discharge (or rest) the data collected so far is
  // a valid partial result, so it completes with `reason`; during priming it
  // cancels via onError. Either way the UI always gets exactly one callback.
  void stop(const char *reason = "Stopped manually") {
    if (!running()) return;
    if (state_ == DISCHARGING) {
      stopReason_ = reason;
      endV_ = vNow_;
      reboundV_ = vNow_;
      endMs_ = millis();
      finishSafely();
      complete();
    } else if (state_ == RESTING) {
      finishSafely();
      complete();
    } else {  // PRIMING
      finishSafely();
      state_ = IDLE;
      if (onError) onError("Cancelled.");
    }
  }

  // Feed a live reading into the running test.
  void onStatus(const el15::Status &s) {
    if (!running() || !s.valid) return;
    vNow_ = s.voltage;
    if (state_ == PRIMING) {
      vOc_ = s.voltage;
      return;
    }
    if (state_ == RESTING) {
      reboundV_ = s.voltage;
      if (onProgress) onProgress(s.voltage, s.current, ah_, wh_, s.temperature,
                                 (endMs_ - tStart_) / 1000, 2);
      return;
    }
    // DISCHARGING
    if (s.warning[0] != '\0') { stopReason_ = "Protection tripped"; enterRest(); return; }
    uint32_t now = millis();
    if (lastMs_) {
      float dtH = (now - lastMs_) / 3600000.0f;
      if (dtH > 0 && dtH < 0.01f) {  // ignore >36 s gaps (link stall / glitch)
        ah_ += s.current * dtH;
        wh_ += s.voltage * s.current * dtH;
        sumVdt_ += s.voltage * dtH;
        sumDt_ += dtH;
      }
    }
    lastMs_ = now;
    if (!haveSample_) { haveSample_ = true; minT_ = maxT_ = s.temperature; }
    else { minT_ = min(minT_, s.temperature); maxT_ = max(maxT_, s.temperature); }
    fanMax_ = max(fanMax_, s.fanSpeed);

    if (onProgress) onProgress(s.voltage, s.current, ah_, wh_, s.temperature,
                               (now - tStart_) / 1000, 1);

    // Debounced automatic cutoff: three consecutive samples at/below the stop
    // point, or a single sample well below it (fail-safe against noise).
    if (s.voltage <= cutoffV) below_++; else below_ = 0;
    if (below_ >= 3 || s.voltage < cutoffV - 0.3f) {
      stopReason_ = "Cutoff voltage reached";
      enterRest();
      return;
    }
    if (ah_ >= maxAh) { stopReason_ = "Capacity cap reached"; enterRest(); return; }
  }

  // Pump timers + duration cap; call from loop().
  void tick() {
    if (state_ == DISCHARGING && (millis() - tStart_) / 1000 >= maxDurationS) {
      stopReason_ = "Max duration reached";
      enterRest();
      return;
    }
    if (state_ == IDLE || timerCb_ == NONE) return;
    if ((int32_t)(millis() - timerAt_) < 0) return;
    TimerCb cb = timerCb_;
    timerCb_ = NONE;
    switch (cb) {
      case PRIME_DONE: finishPriming(); break;
      case REST_DONE: complete(); break;
      default: break;
    }
  }

 private:
  enum State { IDLE, PRIMING, DISCHARGING, RESTING };
  enum TimerCb { NONE, PRIME_DONE, REST_DONE };

  void finishPriming() {
    float voc = vOc_;
    if (voc < el15::MIN_VOLTAGE_V) { abort_("No reading, or voltage below the minimum. Test aborted."); return; }
    if (voc > el15::MAX_VOLTAGE_V) { abort_("Source above the EL15's 60 V rating. Test aborted."); return; }
    if (voc <= cutoffV + 0.2f) { abort_("Battery is already at or below the cutoff voltage."); return; }
    effA_ = min(min(dischargeA, el15::MAX_CURRENT_A), el15::MAX_POWER_W / voc);
    if (effA_ < 0.01f) { abort_("Safe discharge current too low."); return; }
    startV_ = voc;
    below_ = 0;
    lastMs_ = 0;
    state_ = DISCHARGING;
    tStart_ = millis();
    ble_->setSetpoint(effA_);
    ble_->setLoad(true);
  }

  void enterRest() {
    finishSafely();
    endV_ = vNow_;
    reboundV_ = vNow_;
    endMs_ = millis();
    if (restS == 0) { complete(); return; }
    state_ = RESTING;
    timerAt_ = millis() + restS * 1000;
    timerCb_ = REST_DONE;
  }

  void complete() {
    finishSafely();
    state_ = IDLE;
    Result r;
    r.capacityAh = ah_;
    r.energyWh = wh_;
    r.durationS = (endMs_ - tStart_) / 1000;
    r.startV = startV_;
    r.endV = endV_;
    r.reboundV = reboundV_;
    r.avgV = sumDt_ > 1e-9f ? sumVdt_ / sumDt_ : 0;
    r.avgI = sumDt_ > 1e-9f ? ah_ / sumDt_ : 0;
    r.minTemp = haveSample_ ? minT_ : 0;
    r.maxTemp = haveSample_ ? maxT_ : 0;
    r.maxFan = fanMax_;
    r.cutoffV = cutoffV;
    r.currentA = effA_;
    r.stopReason = stopReason_[0] ? stopReason_ : "Completed";
    if (onComplete) onComplete(r);
  }

  void abort_(const char *msg) {
    finishSafely();
    state_ = IDLE;
    if (onError) onError(msg);
  }

  void finishSafely() {
    timerCb_ = NONE;
    ble_->setLoad(false);
    ble_->setSetpoint(0);
  }

  El15Controller *ble_;
  State state_ = IDLE;
  TimerCb timerCb_ = NONE;
  uint32_t timerAt_ = 0, tStart_ = 0, endMs_ = 0, lastMs_ = 0;

  float ah_ = 0, wh_ = 0, sumVdt_ = 0, sumDt_ = 0;
  float vOc_ = 0, vNow_ = 0, startV_ = 0, endV_ = 0, reboundV_ = 0, effA_ = 0;
  float minT_ = 0, maxT_ = 0;
  int fanMax_ = 0, below_ = 0;
  bool haveSample_ = false;
  const char *stopReason_ = "";
};
