// Fuse-aware circuit-resistance test — C++ port of CircuitResistanceTester.
//
// Drives the load in CC mode through a stepped current ladder derived from the
// circuit's fuse rating, samples V and I at each step, and computes the series
// resistance as the slope of the V-I line: V = Voc - R*I, so R = -dV/dI.
//
// Timer-driven state machine, pumped by tick() from loop() and fed live
// readings via onStatus(). Every commanded current is clamped to the EL15's
// 150 W / 60 V / 12 A ratings.
#pragma once

#include <Arduino.h>
#include <math.h>
#include <functional>
#include <vector>
#include "el15_controller.h"
#include "el15_protocol.h"

class ResistanceTest {
 public:
  struct Sample { float current, voltage, temperature; int fanSpeed; };

  struct Result {
    std::vector<Sample> samples;
    float resistanceOhm = 0, openCircuitVoltage = 0, rSquared = 0;
    float fuseRating = 0, maxTestCurrent = 0;
    bool reliable = false;
  };

  // Callbacks (fired from tick(), main task).
  std::function<void(int step, int total, float target, float v, float i)> onProgress;
  std::function<void(const Result &)> onComplete;
  std::function<void(const char *)> onError;

  // Tunables (defaults match the Android app).
  int steps = 8;
  float safetyFactor = 0.8f;
  uint32_t settleMs = 800;
  uint32_t collectMs = 1500;
  uint32_t pollIntervalMs = 500;

  static const int MAX_STEPS = 1000;

  explicit ResistanceTest(El15Controller *ctrl) : ble_(ctrl) {}

  bool running() const { return state_ != IDLE; }

  void start(float fuseRatingAmps) {
    if (running()) return;
    fuseRating_ = fuseRatingAmps;
    results_.clear(); stepBuffer_.clear(); targets_.clear();
    stepIndex_ = 0; vOcMeasured_ = 0; vRecent_ = 0;
    state_ = PRIMING;
    ble_->setMode(el15::MODE_CC);
    ble_->setSetpoint(0);
    ble_->setLoad(false);
    timerAt_ = millis() + max(primeMs(), 2 * pollIntervalMs + 300);
    timerCb_ = PRIME_DONE;
  }

  void stop() {
    if (!running()) return;
    finishSafely();
    state_ = IDLE;
  }

  // Feed a live reading into the running test.
  void onStatus(const el15::Status &s) {
    if (!running() || !s.valid) return;
    if (s.warning[0] != '\0') { abort_("Load protection tripped. Test stopped."); return; }
    vRecent_ = s.voltage;
    if (state_ == PRIMING) vOcMeasured_ = s.voltage;
    else if (state_ == COLLECTING)
      stepBuffer_.push_back({s.current, s.voltage, s.temperature, s.fanSpeed});
  }

  // Pump timers; call from loop().
  void tick() {
    if (state_ == IDLE || timerCb_ == NONE) return;
    if ((int32_t)(millis() - timerAt_) < 0) return;
    TimerCb cb = timerCb_;
    timerCb_ = NONE;
    switch (cb) {
      case PRIME_DONE: finishPriming(); break;
      case SETTLE_DONE:
        stepBuffer_.clear();
        state_ = COLLECTING;
        timerAt_ = millis() + effectiveCollectMs();
        timerCb_ = COLLECT_DONE;
        break;
      case COLLECT_DONE: endStep(); break;
      default: break;
    }
  }

 private:
  enum State { IDLE, PRIMING, SETTLING, COLLECTING };
  enum TimerCb { NONE, PRIME_DONE, SETTLE_DONE, COLLECT_DONE };

  static const uint32_t PRIME_MS = 900;
  static constexpr float MIN_TEST_CURRENT = 0.05f;
  static constexpr float ABS_MAX_CURRENT = 40.0f;
  static constexpr float MIN_TEST_C = 0.05f;

  uint32_t primeMs() { return max(PRIME_MS, 2 * pollIntervalMs + 300); }
  uint32_t effectiveSettleMs() { return max(settleMs, pollIntervalMs + 100); }
  uint32_t effectiveCollectMs() { return max(collectMs, 2 * pollIntervalMs + 200); }

  void finishPriming() {
    float voc = vOcMeasured_;
    if (voc < el15::MIN_VOLTAGE_V) { abort_("No reading, or voltage below the minimum. Test aborted."); return; }
    if (voc > el15::MAX_VOLTAGE_V) { abort_("Source above the EL15's 60 V rating. Test aborted."); return; }
    float fuseCap = fuseRating_ * safetyFactor;
    float powerCap = el15::MAX_POWER_W / voc;
    maxCurrent_ = min(min(fuseCap, el15::MAX_CURRENT_A), min(powerCap, ABS_MAX_CURRENT));
    if (maxCurrent_ < MIN_TEST_CURRENT) { abort_("Safe test current too low - check the fuse rating."); return; }
    int n = constrain(steps, 2, MAX_STEPS);
    for (int k = 1; k <= n; k++) targets_.push_back(maxCurrent_ * k / n);
    beginStep(0);
  }

  float clampToRatings(float target) {
    float v = vRecent_ > el15::MIN_VOLTAGE_V ? vRecent_ : el15::MAX_VOLTAGE_V;
    float powerCap = el15::MAX_POWER_W / v;
    return min(min(target, el15::MAX_CURRENT_A), min(powerCap, ABS_MAX_CURRENT));
  }

  void beginStep(int idx) {
    if ((size_t)idx >= targets_.size()) { complete(); return; }
    stepIndex_ = idx;
    currentTarget_ = clampToRatings(targets_[idx]);
    ble_->setSetpoint(currentTarget_);
    if (idx == 0) ble_->setLoad(true);
    state_ = SETTLING;
    timerAt_ = millis() + effectiveSettleMs();
    timerCb_ = SETTLE_DONE;
  }

  void endStep() {
    state_ = SETTLING;
    float target = currentTarget_;
    if (!stepBuffer_.empty()) {
      double si = 0, sv = 0, st = 0; int fan = 0;
      for (auto &s : stepBuffer_) { si += s.current; sv += s.voltage; st += s.temperature; fan = max(fan, s.fanSpeed); }
      int n = stepBuffer_.size();
      Sample avg{(float)(si / n), (float)(sv / n), (float)(st / n), fan};
      results_.push_back(avg);
      if (onProgress) onProgress(stepIndex_ + 1, targets_.size(), target, avg.voltage, avg.current);
    }
    beginStep(stepIndex_ + 1);
  }

  void complete() {
    if (results_.size() < 2) { abort_("Too few samples - increase the sample window or lower the poll interval."); return; }
    finishSafely();
    state_ = IDLE;
    if (onComplete) onComplete(computeResult());
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

  Result computeResult() {
    Result r;
    r.samples = results_;
    r.fuseRating = fuseRating_;
    r.maxTestCurrent = maxCurrent_;
    int n = results_.size();
    double sumI = 0, sumV = 0;
    for (auto &s : results_) { sumI += s.current; sumV += s.voltage; }
    double meanI = sumI / n, meanV = sumV / n;
    double sII = 0, sIV = 0, sVV = 0;
    for (auto &s : results_) {
      double di = s.current - meanI, dv = s.voltage - meanV;
      sII += di * di; sIV += di * dv; sVV += dv * dv;
    }
    double slope = sII > 1e-9 ? sIV / sII : 0;
    double intercept = meanV - slope * meanI;
    r.resistanceOhm = (float)max(-slope, 0.0);
    r.openCircuitVoltage = (float)intercept;
    r.rSquared = (sII > 1e-9 && sVV > 1e-9) ? (float)((sIV * sIV) / (sII * sVV)) : 0;
    float spread = 0;
    if (n > 0) {
      float mn = results_[0].current, mx = results_[0].current;
      for (auto &s : results_) { mn = min(mn, s.current); mx = max(mx, s.current); }
      spread = mx - mn;
    }
    r.reliable = n >= 3 && slope < 0 && r.rSquared >= 0.90f && spread > 0.05f;
    return r;
  }

  El15Controller *ble_;
  State state_ = IDLE;
  TimerCb timerCb_ = NONE;
  uint32_t timerAt_ = 0;

  std::vector<float> targets_;
  std::vector<Sample> results_;
  std::vector<Sample> stepBuffer_;
  int stepIndex_ = 0;
  float fuseRating_ = 0, maxCurrent_ = 0, currentTarget_ = 0;
  float vOcMeasured_ = 0, vRecent_ = 0;
};
