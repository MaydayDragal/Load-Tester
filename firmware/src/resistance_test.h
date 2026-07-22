// Fuse-aware circuit-resistance test — C++ port of CircuitResistanceTester.
//
// Drives the load in CC mode through a stepped current ladder derived from the
// circuit's fuse rating, samples V and I at each step, and computes the series
// resistance as the slope of the V-I line: V = Voc - R*I, so R = -dV/dI.
//
// Accuracy techniques (see RTEST_ACCURACY.md):
//  1. BIDIRECTIONAL sweep — the ladder is walked up then back down (a triangle
//     over the distinct current levels) and the two visits to each level are
//     averaged. Because the two visits are symmetric about the sweep midpoint,
//     first-order time drift (wire self-heating, battery sag) cancels instead
//     of biasing the slope, which a one-way ramp does.
//  2. UNCERTAINTY — reports the 1-sigma standard error of the fitted slope
//     (R +/- sigma), and gates "reliable" on that tolerance rather than R^2,
//     which is meaningful at any resistance scale.
//  3. SAMPLE DENSITY — the collect window scales with the poll interval to
//     gather ~10 readings per step (capped), so a higher Settings sample rate
//     directly tightens the fit.
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
    float resistanceStdErr = 0;   // 1-sigma uncertainty on resistanceOhm (ohm)
    float fuseRating = 0, maxTestCurrent = 0;
    bool reliable = false;
    // What the slope actually measured, before the lead tare was taken off.
    // rawResistanceOhm is also what a tare run itself records.
    float rawResistanceOhm = 0;
    float tareOhm = 0;            // subtracted (0 in 4-wire, where there is nothing to subtract)
    bool fourWire = false;
  };

  // Callbacks (fired from tick(), main task).
  std::function<void(int step, int total, float target, float v, float i)> onProgress;
  std::function<void(const Result &)> onComplete;
  std::function<void(const char *)> onError;

  // Probe wiring. In 4-wire (Kelvin) sensing the voltage is measured through a
  // second pair of leads that carry no current, so lead and contact resistance
  // never enters the reading and there is nothing to subtract. In 2-wire it is
  // measured in series with the circuit under test, so `tareOhm` — captured by
  // running a sweep with the probes shorted together — is taken off the result.
  // At the milliohm scale that correction is often larger than what is being
  // measured, which is why this matters.
  bool fourWire = false;
  float tareOhm = 0;

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
    results_.clear(); stepBuffer_.clear();
    levels_.clear(); seq_.clear(); levelAcc_.clear();
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

  // Aim for ~TARGET_SAMPLES readings per step so each averaged point is well
  // determined; scale with the poll interval but cap so a slow rate can't make
  // the sweep interminable. A faster Settings sample rate → more samples/step →
  // tighter fit (technique 3).
  static const int TARGET_SAMPLES = 10;
  static const uint32_t COLLECT_CAP_MS = 2500;

  uint32_t primeMs() { return max(PRIME_MS, 2 * pollIntervalMs + 300); }
  uint32_t effectiveSettleMs() { return max(settleMs, pollIntervalMs + 100); }
  uint32_t effectiveCollectMs() {
    uint32_t want = (uint32_t)TARGET_SAMPLES * pollIntervalMs + 100;
    uint32_t floorMs = max((uint32_t)1000, 2 * pollIntervalMs + 200);
    return min(max(want, floorMs), COLLECT_CAP_MS);
  }

  void finishPriming() {
    float voc = vOcMeasured_;
    if (voc < el15::MIN_VOLTAGE_V) { abort_("No reading, or voltage below the minimum. Test aborted."); return; }
    if (voc > el15::MAX_VOLTAGE_V) { abort_("Source above the EL15's 60 V rating. Test aborted."); return; }
    float fuseCap = fuseRating_ * safetyFactor;
    float powerCap = el15::MAX_POWER_W / voc;
    maxCurrent_ = min(min(fuseCap, el15::MAX_CURRENT_A), min(powerCap, ABS_MAX_CURRENT));
    if (maxCurrent_ < MIN_TEST_CURRENT) { abort_("Safe test current too low - check the fuse rating."); return; }
    int n = constrain(steps, 2, MAX_STEPS);
    for (int k = 1; k <= n; k++) levels_.push_back(maxCurrent_ * k / n);
    // Bidirectional triangle over the levels: up 0..n-1 then down n-2..0. Each
    // level except the apex is visited twice, symmetrically about the sweep
    // midpoint, so first-order time drift cancels when the two visits average
    // (technique 1). The apex sits at the midpoint already (single visit).
    for (int k = 0; k < n; k++) seq_.push_back(k);
    for (int k = n - 2; k >= 0; k--) seq_.push_back(k);
    levelAcc_.assign(n, LevelAcc{});
    beginStep(0);
  }

  float clampToRatings(float target) {
    float v = vRecent_ > el15::MIN_VOLTAGE_V ? vRecent_ : el15::MAX_VOLTAGE_V;
    float powerCap = el15::MAX_POWER_W / v;
    return min(min(target, el15::MAX_CURRENT_A), min(powerCap, ABS_MAX_CURRENT));
  }

  void beginStep(int idx) {
    if ((size_t)idx >= seq_.size()) { complete(); return; }
    stepIndex_ = idx;
    currentTarget_ = clampToRatings(levels_[seq_[idx]]);
    ble_->setSetpoint(currentTarget_);
    if (idx == 0) ble_->setLoad(true);
    state_ = SETTLING;
    timerAt_ = millis() + effectiveSettleMs();
    timerCb_ = SETTLE_DONE;
  }

  void endStep() {
    state_ = SETTLING;
    if (!stepBuffer_.empty()) {
      double si = 0, sv = 0, st = 0; int fan = 0;
      for (auto &s : stepBuffer_) { si += s.current; sv += s.voltage; st += s.temperature; fan = max(fan, s.fanSpeed); }
      int m = stepBuffer_.size();
      double avgI = si / m, avgV = sv / m, avgT = st / m;
      // Bin this visit into its current level; the two visits per level average
      // together at complete(), cancelling drift.
      LevelAcc &a = levelAcc_[seq_[stepIndex_]];
      a.sumI += avgI; a.sumV += avgV; a.sumT += avgT; a.count++; a.fanMax = max(a.fanMax, fan);
      if (onProgress) onProgress(stepIndex_ + 1, (int)seq_.size(), currentTarget_, (float)avgV, (float)avgI);
    }
    beginStep(stepIndex_ + 1);
  }

  void complete() {
    // One drift-cancelled point per visited level, in ascending current order.
    results_.clear();
    for (auto &a : levelAcc_)
      if (a.count > 0)
        results_.push_back(Sample{(float)(a.sumI / a.count), (float)(a.sumV / a.count),
                                  (float)(a.sumT / a.count), a.fanMax});
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
    r.rawResistanceOhm = (float)max(-slope, 0.0);
    // 4-wire already excludes the leads; 2-wire subtracts the measured tare and
    // clamps at zero (a DUT below the tare means the tare is stale, not that
    // resistance went negative).
    r.fourWire = fourWire;
    r.tareOhm = fourWire ? 0 : tareOhm;
    r.resistanceOhm = max(r.rawResistanceOhm - r.tareOhm, 0.0f);
    r.openCircuitVoltage = (float)intercept;
    r.rSquared = (sII > 1e-9 && sVV > 1e-9) ? (float)((sIV * sIV) / (sII * sVV)) : 0;

    // 1-sigma standard error of the slope (technique 2): a real +/- tolerance on
    // R. SSE = S_VV - slope*S_IV (residual sum of squares); Var(slope) =
    // (SSE/(n-2)) / S_II. Needs >= 3 points for a degree of freedom.
    double sse = sVV - slope * sIV;
    if (sse < 0) sse = 0;
    double stdErr = (n > 2 && sII > 1e-9) ? sqrt(sse / ((n - 2) * sII)) : 0;
    r.resistanceStdErr = (float)stdErr;

    float spread = 0;
    if (n > 0) {
      float mn = results_[0].current, mx = results_[0].current;
      for (auto &s : results_) { mn = min(mn, s.current); mx = max(mx, s.current); }
      spread = mx - mn;
    }
    // Reliable when the uncertainty is small in ABSOLUTE (<= 5 mohm) OR RELATIVE
    // (<= 5 %) terms — meaningful at any resistance scale, unlike R^2 which
    // false-alarms on clean low-milliohm reads and rubber-stamps noisy big ones.
    // Judge the uncertainty against what was MEASURED, not against the
    // tare-corrected figure: subtracting a constant cannot make the fit better,
    // and a near-tare result would otherwise look wildly unreliable.
    double relTol = r.rawResistanceOhm > 1e-4 ? stdErr / r.rawResistanceOhm : 1e9;
    r.reliable = n >= 3 && slope < 0 && spread > 0.05f &&
                 (stdErr <= 0.005 || relTol <= 0.05);
    return r;
  }

  El15Controller *ble_;
  State state_ = IDLE;
  TimerCb timerCb_ = NONE;
  uint32_t timerAt_ = 0;

  // Per-level accumulator: sums the (1 or 2) visits to a current level so they
  // average into one drift-cancelled point.
  struct LevelAcc { double sumI = 0, sumV = 0, sumT = 0; int count = 0, fanMax = 0; };

  std::vector<float> levels_;        // distinct current setpoints, ascending
  std::vector<int> seq_;             // physical visit order (indices into levels_)
  std::vector<LevelAcc> levelAcc_;   // per-level sums, one entry per level
  std::vector<Sample> results_;      // one averaged sample per level (built at complete)
  std::vector<Sample> stepBuffer_;   // raw samples within the current collect window
  int stepIndex_ = 0;                // index into seq_
  float fuseRating_ = 0, maxCurrent_ = 0, currentTarget_ = 0;
  float vOcMeasured_ = 0, vRecent_ = 0;
};
