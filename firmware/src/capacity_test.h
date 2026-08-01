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
#include "battery_model.h"
#include "el15_controller.h"
#include "el15_protocol.h"
#include "sample_log.h"

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
    // Rated-capacity derived figures. ratedAh == 0 means the user did not enter
    // a rating, and sohPct/ratedPct are then meaningless and not shown.
    float ratedAh = 0;
    float sohPct = 0;      // measured / rated x 100 — state of health
    float cRate = 0;       // discharge current expressed in C
    uint32_t pausedS = 0;  // total time spent paused (excluded from durationS)
    // Battery-model figures. All are 0 / -1 when the chemistry carries no
    // voltage curve (Custom) or the model never established itself.
    // internalResistanceOhm is measured from the switch-on sag, so it covers
    // everything between the sense point and the cells: with probe compensation
    // active (4-wire, or a 2-wire lead tare — see main.cpp compensateProbe) the
    // leads are already out of the reading and this is the pack alone; without
    // it, it is pack + contacts + leads.
    float internalResistanceOhm = 0;
    float startSocPct = -1;           // state of charge when the discharge began
    float endSocPct = -1;             // ... and when it stopped
    // Full-pack capacity implied by this run: Ah drawn per unit of state of
    // charge travelled, extrapolated to the whole 0-100 % range. Meaningful even
    // for a PARTIAL discharge, which the measured capacityAh above is not.
    float impliedFullAh = 0;
  };

  // Configuration — set before start().
  float cutoffV = 0;                     // automatic minimum-voltage stop point
  float dischargeA = 0;                  // requested discharge current
  float ratedAh = 0;                     // optional nameplate capacity (0 = unknown)
  uint32_t maxDurationS = 12u * 3600u;   // safety cap (counts ACTIVE time only)
  float maxAh = 50;                      // safety cap
  uint32_t restS = 60;                   // post-cutoff rebound window (0 = none)
  // Battery model for the time-remaining estimate: an index into
  // battmodel::CHEMS and the pack's series cell count. A chemistry with no
  // voltage curve (Custom), or a cell count of 0, leaves the engine on the old
  // rated-capacity estimate — nothing else changes.
  int chemistry = -1;
  int cells = 0;

  // Callbacks (fired on the loop task).
  // phase: 1 = discharging, 2 = resting, 3 = paused.
  std::function<void(float v, float i, float ah, float wh, float temp,
                     uint32_t elapsedS, int phase)> onProgress;
  std::function<void(const Result &)> onComplete;
  std::function<void(const char *)> onError;
  // Pause/resume transitions, so the UI can explain itself. `reason` is a static
  // string while paused, nullptr on resume.
  std::function<void(bool paused, const char *reason)> onPause;

  explicit CapacityTest(El15Controller *ctrl) : ble_(ctrl) {}

  bool running() const { return state_ != IDLE; }
  bool paused() const { return state_ == PAUSED; }
  const char *pauseReason() const { return pauseReason_; }

  // Seconds of ACTIVE discharge so far (paused time excluded).
  uint32_t elapsedS() const {
    if (state_ == IDLE || tStart_ == 0) return 0;
    uint32_t end = (state_ == RESTING || state_ == IDLE) ? endMs_ : millis();
    uint32_t paused = pausedMs_ + (state_ == PAUSED ? millis() - pauseStartMs_ : 0);
    return (end - tStart_ - paused) / 1000;
  }

  // Estimated seconds of discharge left. 0 = no estimate available.
  //
  // Preferred path (a chemistry with a voltage curve): how much state of charge
  // is still to be travelled before the CUTOFF is reached, scaled by the pack's
  // capacity. That capacity is learned from this very run — Ah drawn per unit of
  // SoC travelled — so the estimate stops depending on the curve's absolute
  // calibration within the first few minutes, and works with no nameplate rating
  // entered at all.
  //
  // Fallback (Custom chemistry, unknown cell count, or before the model has
  // established itself): the old rated-capacity estimate. It assumes the pack
  // started full and that the run ends at the rating rather than at the cutoff,
  // which is exactly what the curve path exists to fix.
  uint32_t remainingS() const {
    if (state_ != DISCHARGING || effA_ <= 0.001f) return 0;
    if (socValid_) {
      float q = qLearned_ > 0 ? qLearned_ : ratedAh;
      if (q > 0) {
        float left = (socNow_ - socCut_) * q;
        if (left <= 0) return 0;
        return (uint32_t)(left / effA_ * 3600.0f);
      }
    }
    if (ratedAh <= 0) return 0;
    float left = ratedAh - ah_;
    if (left <= 0) return 0;
    return (uint32_t)(left / effA_ * 3600.0f);
  }

  // Live battery-model readouts for the UI. socPct is < 0 until the model has
  // measured the pack's internal resistance and taken its first reading, which
  // takes a few seconds of discharge.
  float socPct() const { return socValid_ ? socNow_ * 100.0f : -1.0f; }
  // True when remainingS() is coming from the discharge curve rather than from
  // the nameplate rating — the UI says which, because they are not equally
  // trustworthy and the user should know what they are looking at.
  bool etaFromCurve() const {
    return socValid_ && (qLearned_ > 0 || ratedAh > 0);
  }
  // Pack + contact + lead resistance measured from the switch-on sag (0 = not
  // measured yet). A useful number in its own right: it is most of what
  // separates a tired pack from a healthy one at the same capacity.
  float internalResistanceOhm() const { return rIntDone_ ? rInt_ : 0.0f; }

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
    pausedMs_ = 0; pauseStartMs_ = 0; pauseReason_ = "";
    tStart_ = 0; endMs_ = 0;
    rInt_ = 0; rIntSum_ = 0; rIntN_ = 0; rIntDone_ = false;
    socNow_ = socStart_ = socCut_ = 0; socValid_ = false;
    ahAtSocStart_ = 0; qLearned_ = 0; lastSocMs_ = 0;
    // Open the flash-backed datapoint log. A failure here is non-fatal: the
    // test still runs and reports, the CSV just carries no per-sample block.
    logging_ = samplelog::start();
    state_ = PRIMING;
    ble_->setMode(el15::MODE_CC);
    ble_->setSetpoint(0);
    ble_->setLoad(false);
    timerAt_ = millis() + 1500;   // a couple of samples of open-circuit voltage
    timerCb_ = PRIME_DONE;
  }

  // Manual/external stop. Mid-discharge (or rest, or paused) the data collected
  // so far is a valid partial result, so it completes with `reason`; during
  // priming it cancels via onError. Either way the UI always gets exactly one
  // callback.
  void stop(const char *reason = "Stopped manually") {
    if (!running()) return;
    if (state_ == DISCHARGING || state_ == PAUSED) {
      // Close out the paused span and rejoin the normal path, so elapsedS()
      // below counts it exactly once.
      if (state_ == PAUSED) {
        pausedMs_ += millis() - pauseStartMs_;
        state_ = DISCHARGING;
      }
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

  // Suspend the discharge WITHOUT ending the test: the load goes off and the
  // clock stops, but every accumulator (Ah, Wh, min/max, the flash log) is kept
  // so resume() carries straight on. This is what a controller-battery warning
  // or a dropped BLE link should do — those are reasons to stop drawing current,
  // not reasons to throw away hours of measurement.
  //
  // Only a live discharge can pause; returns false otherwise so the caller can
  // fall back to stop(). Priming has nothing worth keeping and resting is
  // already load-off and nearly over.
  bool pause(const char *reason) {
    if (state_ != DISCHARGING) return false;
    pauseStartMs_ = millis();
    pauseReason_ = reason;
    state_ = PAUSED;
    finishSafely();   // LOAD OFF + setpoint 0 — the whole point of pausing
    samplelog::flush();
    Serial.printf("[batt] PAUSED: %s (%.3f Ah so far)\n", reason, ah_);
    if (onPause) onPause(true, reason);
    if (onProgress) onProgress(vNow_, 0, ah_, wh_, lastTemp_, elapsedS(), 3);
    return true;
  }

  // Resume a paused discharge. Returns false if there was nothing to resume.
  bool resume() {
    if (state_ != PAUSED) return false;
    pausedMs_ += millis() - pauseStartMs_;
    pauseReason_ = "";
    // Drop the integration anchor: no Ah may be accrued across the paused gap,
    // and the load needs a moment to re-regulate before its readings count.
    lastMs_ = 0;
    below_ = 0;
    state_ = DISCHARGING;
    ble_->setSetpoint(effA_);
    ble_->setLoad(true);
    Serial.printf("[batt] RESUMED at %.3f A (%.3f Ah so far)\n", effA_, ah_);
    if (onPause) onPause(false, nullptr);
    return true;
  }

  // Feed a live reading into the running test.
  void onStatus(const el15::Status &s) {
    if (!running() || !s.valid) return;
    vNow_ = s.voltage;
    lastTemp_ = s.temperature;
    if (state_ == PRIMING) {
      vOc_ = s.voltage;
      return;
    }
    if (state_ == PAUSED) {
      // Still worth showing the (now unloaded) voltage recovering, but nothing
      // is integrated and the elapsed clock is frozen.
      if (onProgress) onProgress(s.voltage, s.current, ah_, wh_, s.temperature,
                                 elapsedS(), 3);
      return;
    }
    if (state_ == RESTING) {
      reboundV_ = s.voltage;
      if (onProgress) onProgress(s.voltage, s.current, ah_, wh_, s.temperature,
                                 elapsedS(), 2);
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
    updateModel(s, now);

    uint32_t el = elapsedS();
    // Offer every reading to the flash log; its tier schedule decides which are
    // actually stored, so this stays cheap at a 20 Hz poll rate.
    if (logging_) samplelog::add(el, s.voltage, s.current, s.temperature, ah_, wh_);

    if (onProgress) onProgress(s.voltage, s.current, ah_, wh_, s.temperature,
                               el, 1);

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
    // The cap counts ACTIVE discharge time, so a long pause can't end the test
    // by itself — but the total energy budget it guards is unchanged.
    if (state_ == DISCHARGING && elapsedS() >= maxDurationS) {
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
  enum State { IDLE, PRIMING, DISCHARGING, PAUSED, RESTING };
  enum TimerCb { NONE, PRIME_DONE, REST_DONE };

  // ---- Battery model -------------------------------------------------------
  // Window over which the switch-on sag is averaged into an internal-resistance
  // figure. It starts late enough for the load to be regulating at the commanded
  // current and ends before the pack has meaningfully discharged.
  static const uint32_t IR_WINDOW_START_MS = 1500;
  static const uint32_t IR_WINDOW_END_MS = 5000;
  static const int IR_MIN_SAMPLES = 3;            // quorum before R is trusted
  static constexpr float IR_MAX_OHM = 20.0f;      // beyond this it is a bad reading, not a pack
  static constexpr float SOC_TAU_S = 30.0f;       // state-of-charge filter time constant
  static constexpr float SOC_LEARN_MIN = 0.10f;   // SoC travel before capacity is learned

  bool haveCurve() const {
    return battmodel::hasCurve(chemistry) && cells > 0;
  }

  // Internal resistance, state of charge, and the pack capacity implied by the
  // two together. Called once per discharging sample.
  //
  // The voltage the load reports is the cells' open-circuit voltage minus I*R of
  // the pack, its contacts and the test leads, so it cannot be looked up on a
  // RESTED voltage curve directly — at 2 A through 150 mohm that is 0.3 V, which
  // on Li-ion is most of the difference between 40 % and 70 % charged. Priming
  // gives the open-circuit voltage and the first seconds of discharge give the
  // loaded voltage at a known current, so R falls straight out of the two.
  //
  // Using the SAME R at both ends is what makes the estimate self-consistent:
  // the engine cuts on the LOADED voltage, so the state of charge the run will
  // actually stop at is the one at (cutoff + I*R), and any lead resistance in R
  // therefore shifts both the current reading and the target together instead of
  // biasing the time between them.
  void updateModel(const el15::Status &s, uint32_t now) {
    if (!haveCurve()) return;

    if (!rIntDone_) {
      // Active discharge time, not wall clock: a pause in the first seconds
      // would otherwise run the window out while the load was off.
      uint32_t since = now - tStart_ - pausedMs_;
      // Only sample once the load is actually regulating at the commanded
      // current — a reading taken while it is still ramping in would divide a
      // real sag by a fraction of the real current and overstate R badly.
      if (since >= IR_WINDOW_START_MS && s.current > effA_ * 0.8f) {
        float r = (startV_ - s.voltage) / s.current;
        if (r > 0 && r < IR_MAX_OHM) { rIntSum_ += r; rIntN_++; }
      }
      // Close the window on TIME AND a quorum of samples. Requiring both means a
      // run whose first seconds were lost (a pause, or a load slow to regulate)
      // measures R late rather than not at all — and a load that never reaches
      // the commanded current never fabricates one, which correctly leaves the
      // estimate on its rated-capacity fallback.
      if (since < IR_WINDOW_END_MS || rIntN_ < IR_MIN_SAMPLES) return;
      rInt_ = rIntSum_ / rIntN_;
      rIntDone_ = true;
      socCut_ = battmodel::socFromOcv(chemistry, (cutoffV + effA_ * rInt_) / (float)cells);
      if (socCut_ < 0) socCut_ = 0;
      Serial.printf("[batt] pack+lead resistance %.1f mohm (%d samples); cutoff is ~%.0f%% SoC\n",
                    rInt_ * 1000.0f, rIntN_, socCut_ * 100.0f);
      return;
    }

    float soc = battmodel::socFromOcv(chemistry,
                                      (s.voltage + s.current * rInt_) / (float)cells);
    if (soc < 0) return;
    if (!socValid_) {
      socNow_ = socStart_ = soc;
      ahAtSocStart_ = ah_;   // the first few seconds of Ah predate this anchor
      socValid_ = true;
      lastSocMs_ = now;
      Serial.printf("[batt] start of charge ~%.0f%% (%s, %dS)\n",
                    socStart_ * 100.0f, battmodel::CHEMS[chemistry].name, cells);
    } else {
      // Time-constant filter rather than a fixed per-sample coefficient: the
      // sample rate is a user setting (2-20 Hz), and a fixed coefficient would
      // make the filter ten times slower at the bottom of that range. A gap
      // (link stall, a resume after a pause) is skipped rather than integrated.
      float dt = (now - lastSocMs_) / 1000.0f;
      lastSocMs_ = now;
      if (dt > 0 && dt < 10.0f) socNow_ += (soc - socNow_) * (dt / (dt + SOC_TAU_S));
    }

    // Learn the pack's real capacity: Ah out per unit of state of charge
    // travelled. This is the step that turns "trust the curve" into a
    // measurement — after the first 10 % of travel the curve only has to get the
    // SHAPE right, not the absolute calibration, and an estimate becomes
    // possible with no nameplate rating entered at all.
    float travelled = socStart_ - socNow_;
    if (travelled > SOC_LEARN_MIN) {
      float q = (ah_ - ahAtSocStart_) / travelled;
      // A nameplate rating is not authoritative — measuring how far it is out is
      // the point of the test — but it does bound what is a plausible reading
      // versus arithmetic on noise.
      bool sane = q > 0.001f && q < 2000.0f;
      if (sane && ratedAh > 0) sane = q > ratedAh * 0.05f && q < ratedAh * 5.0f;
      if (sane) qLearned_ = q;
    }
  }

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
    uint32_t activeS = elapsedS();
    uint32_t pausedS = pausedMs_ / 1000;
    state_ = IDLE;
    samplelog::flush();   // the last partial batch belongs in the report
    Result r;
    r.capacityAh = ah_;
    r.energyWh = wh_;
    r.durationS = activeS;
    r.pausedS = pausedS;
    r.ratedAh = ratedAh;
    r.sohPct = ratedAh > 0 ? ah_ / ratedAh * 100.0f : 0;
    r.cRate = ratedAh > 0 && effA_ > 0 ? effA_ / ratedAh : 0;
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
    r.internalResistanceOhm = rIntDone_ ? rInt_ : 0;
    r.startSocPct = socValid_ ? socStart_ * 100.0f : -1;
    r.endSocPct = socValid_ ? socNow_ * 100.0f : -1;
    // Only claim an implied full capacity if one was actually learned. A run
    // stopped after 2 % of travel has not measured anything worth extrapolating.
    r.impliedFullAh = qLearned_;
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
  // Paused-time bookkeeping, so the reported duration and the logged timeline
  // are both ACTIVE discharge time.
  uint32_t pausedMs_ = 0, pauseStartMs_ = 0;
  const char *pauseReason_ = "";

  float ah_ = 0, wh_ = 0, sumVdt_ = 0, sumDt_ = 0;
  float vOc_ = 0, vNow_ = 0, startV_ = 0, endV_ = 0, reboundV_ = 0, effA_ = 0;
  float minT_ = 0, maxT_ = 0, lastTemp_ = 0;
  int fanMax_ = 0, below_ = 0;
  bool haveSample_ = false;
  bool logging_ = false;
  const char *stopReason_ = "";

  // Battery-model state (see updateModel).
  float rInt_ = 0, rIntSum_ = 0;
  int rIntN_ = 0;
  bool rIntDone_ = false;
  float socNow_ = 0, socStart_ = 0, socCut_ = 0;
  bool socValid_ = false;
  float ahAtSocStart_ = 0, qLearned_ = 0;
  uint32_t lastSocMs_ = 0;
};
