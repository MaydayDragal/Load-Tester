// Fuse-aware circuit-resistance test — continuous triangular current sweep.
//
// Drives the load in CC mode along a SMOOTH current ramp: up from a starting
// current to a maximum over half the sweep, then back down over the other half.
// Every status packet that arrives on the way is a data point, and the series
// resistance is the slope of the V-I line through them: V = Voc - R*I, so
// R = -dV/dI.
//
// Why a ramp rather than the discrete ladder this replaced: the old design had
// to sit at each current level for a settle window plus a collect window
// (~2.3 s per level), which bought only ~10 usable readings per level and made
// the sweep length a function of the step count rather than something the user
// could choose. A continuous ramp uses EVERY packet — a 30 s sweep at the 20 Hz
// default yields ~300 points instead of ~80 — and the sweep duration becomes a
// plain, editable number. It also produces a live curve worth watching.
//
// Accuracy techniques (see RTEST_ACCURACY.md):
//  1. BIDIRECTIONAL — the ramp is a symmetric triangle, so each current is
//     visited once on the way up and once on the way down, equally spaced about
//     the sweep midpoint. First-order time drift (wire self-heating, battery
//     sag) therefore cancels in the fit instead of biasing the slope, exactly as
//     the stepped version's up-then-down ladder did.
//  2. UNCERTAINTY — reports the 1-sigma standard error of the fitted slope
//     (R +/- sigma) and gates "reliable" on that tolerance rather than R^2,
//     which is meaningful at any resistance scale.
//  3. SAMPLE DENSITY — every packet counts, so a higher Settings sample rate
//     directly tightens the fit. The regression runs incrementally (running
//     sums), so the live R estimate costs nothing and no sample buffer grows.
//
// V and I inside one status frame are simultaneous, which is what makes a moving
// setpoint safe to fit: the load's regulation lag shifts WHICH current a packet
// reports, not the V that goes with it, so the V-I relationship is undistorted.
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
    std::vector<Sample> samples;   // one binned point per current band
    float resistanceOhm = 0, openCircuitVoltage = 0, rSquared = 0;
    float resistanceStdErr = 0;   // 1-sigma uncertainty on resistanceOhm (ohm)
    float fuseRating = 0, maxTestCurrent = 0;
    float startCurrent = 0;        // commanded ramp floor
    uint32_t sweepSeconds = 0;     // commanded ramp duration
    int rawSamples = 0;            // status packets actually used in the fit
    bool reliable = false;
    // Run statistics, taken over EVERY raw sample rather than the binned curve,
    // so they report what the load actually saw.
    float minCurrent = 0, maxCurrentSeen = 0;
    float sagV = 0;                // full voltage swing across the sweep
    float peakPowerW = 0;
    float tempMin = 0, tempMax = 0;
    int maxFan = 0;
    int loadDropouts = 0;   // times the load had to be re-asserted mid-sweep
    // What the slope actually measured, before the lead tare was taken off.
    // rawResistanceOhm is also what a tare run itself records.
    float rawResistanceOhm = 0;
    float tareOhm = 0;            // subtracted (0 in 4-wire, where there is nothing to subtract)
    bool fourWire = false;
  };

  // Callbacks (fired from tick()/onStatus(), loop task).
  // `rValid` is false until the sweep has covered enough current span for the
  // running fit to mean anything — the UI must not show a number before then.
  std::function<void(float elapsedS, float totalS, float target,
                     float v, float i, float r, bool rValid)> onProgress;
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

  // Sweep shape. startCurrent is where the ramp begins (usually 0) and
  // maxCurrent where it turns around; both are clamped to what the fuse rating
  // and the EL15's ratings allow, so a user asking for too much gets a safe
  // sweep rather than a refusal. maxCurrent <= 0 means "use the whole safe
  // headroom the fuse allows".
  float startCurrent = 0;
  float maxCurrent = 0;
  uint32_t sweepSeconds = 30;

  float safetyFactor = 0.8f;
  uint32_t pollIntervalMs = 50;
  // How often the ramp re-commands the setpoint. 0 = derive it from the poll
  // interval (see setpointStepMs). Every control write defers the next poll, so
  // this trades ramp granularity against how many samples the sweep collects.
  uint32_t setpointMs = 0;
  // Ignore samples for this long after the load is switched on, to skip the
  // regulator's start-up transient. 0 = use everything.
  uint32_t settleMs = 0;

  static const uint32_t MIN_SWEEP_S = 5;
  static const uint32_t MAX_SWEEP_S = 900;

  explicit ResistanceTest(El15Controller *ctrl) : ble_(ctrl) {}

  bool running() const { return state_ != IDLE; }

  // Safe peak current for a given fuse rating and source voltage — the same
  // clamp the sweep applies, exposed so the setup screen can show the user what
  // their fuse actually permits before they start.
  static float safeMaxCurrent(float fuseRating, float voc, float safety = 0.8f) {
    float v = voc > el15::MIN_VOLTAGE_V ? voc : el15::MAX_VOLTAGE_V;
    float powerCap = el15::MAX_POWER_W / v;
    return min(min(fuseRating * safety, el15::MAX_CURRENT_A),
               min(powerCap, ABS_MAX_CURRENT));
  }

  void start(float fuseRatingAmps) {
    if (running()) return;
    fuseRating_ = fuseRatingAmps;
    resetAccumulators();
    vOcMeasured_ = 0; vRecent_ = 0; loadDropouts_ = 0;
    state_ = PRIMING;
    primeStartMs_ = millis();
    lastClearPushMs_ = 0;
    ble_->setMode(el15::MODE_CC);
    ble_->setSetpoint(0);
    ble_->setLoad(false);
    timerAt_ = millis() + primeMs();
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
    vRecent_ = s.voltage;

    if (state_ == PRIMING) {
      // A protection latched by a PREVIOUS run — a link drop or a controller
      // reset that stranded the load, say — must not veto this one forever.
      // Priming already commands mode CC / setpoint 0 / LOAD OFF, which is
      // exactly the state that lets the device clear itself, so keep pushing
      // that and give it a grace period before giving up. Only a trip that
      // survives the grace period is a real reason not to start.
      if (s.warning[0] != '\0') {
        if (millis() - primeStartMs_ > FAULT_CLEAR_MS) {
          abort_("Load protection will not clear. Check the setup, then retry.");
          return;
        }
        if (millis() - lastClearPushMs_ > 400) {
          lastClearPushMs_ = millis();
          Serial.printf("[rtest] clearing latched protection (%s)...\n", s.warning);
          ble_->setLoad(false);
          ble_->setSetpoint(0);
        }
        // Do not let the prime timer expire while the device is still faulted,
        // and do not trust this packet's voltage as an open-circuit reading.
        timerAt_ = millis() + primeMs();
        return;
      }
      vOcMeasured_ = s.voltage;
      return;
    }

    // Mid-sweep a protection trip is real and immediate: current is flowing.
    if (s.warning[0] != '\0') { abort_("Load protection tripped. Test stopped."); return; }
    if (state_ != SWEEPING) return;

    // The load must actually be sinking for a sample to be part of the sweep.
    // A packet reporting the load off is either the gap before LOAD_ON lands or
    // a dropout, and its (0 A, Voc) point would drag the fit toward the
    // intercept while telling us nothing about the slope. Re-assert LOAD_ON
    // rather than silently sweeping a dead load, but no faster than once a
    // second so this cannot become a command flood of its own.
    if (!s.loadOn) {
      if (millis() - lastOnPushMs_ > LOAD_REASSERT_MS) {
        lastOnPushMs_ = millis();
        loadDropouts_++;
        Serial.printf("[rtest] load reported OFF mid-sweep (target %.3f A) - re-asserting\n",
                      currentTarget_);
        ble_->setSetpoint(currentTarget_);
        ble_->setLoad(true);
      }
      if (onProgress)
        onProgress(elapsedS(), (float)sweepMsEff_ / 1000.0f, currentTarget_,
                   s.voltage, s.current, 0, false);
      return;
    }

    // Optional start-up transient skip. The load has to establish regulation
    // from off, and those first readings are of that transient rather than of
    // the ramp. Discarded symmetrically in effect: they are all at the ramp's
    // baseline current, so dropping them costs no span.
    if (settleMs > 0 && millis() - tStart_ < settleMs) {
      if (onProgress)
        onProgress(elapsedS(), (float)sweepMsEff_ / 1000.0f, currentTarget_,
                   s.voltage, s.current, 0, false);
      return;
    }

    // Running least squares. Keeping the six sums rather than the samples means
    // the fit is available at any instant and nothing grows during the sweep —
    // the engine allocates nothing at all between start() and complete().
    double i = s.current, v = s.voltage;
    n_++;
    sumI_ += i; sumV_ += v;
    sumII_ += i * i; sumIV_ += i * v; sumVV_ += v * v;
    if (n_ == 1) { minI_ = maxI_ = (float)i; minV_ = maxV_ = (float)v; }
    else {
      minI_ = min(minI_, (float)i); maxI_ = max(maxI_, (float)i);
      minV_ = min(minV_, (float)v); maxV_ = max(maxV_, (float)v);
    }
    // Seed both bounds from the first sample rather than from zero — a
    // below-zero ambient would otherwise never move tempMax_ off 0.
    if (!haveTemp_) { tempMin_ = tempMax_ = s.temperature; haveTemp_ = true; }
    else { tempMin_ = min(tempMin_, s.temperature); tempMax_ = max(tempMax_, s.temperature); }
    fanMax_ = max(fanMax_, s.fanSpeed);
    peakW_ = max(peakW_, (float)(v * i));

    // Bin by measured current for the result curve and the CSV. Fixed array,
    // so no allocation — and because the ramp visits each bin once going up and
    // once coming down, the two visits average together in the bin, which is
    // where the drift cancellation actually lands.
    binSample(s);

    float r = 0;
    bool ok = fitSlope(r);
    if (onProgress)
      onProgress(elapsedS(), (float)sweepMsEff_ / 1000.0f, currentTarget_,
                 s.voltage, s.current, correct(r), ok);
  }

  // Pump the ramp + timers; call from loop().
  void tick() {
    if (state_ == SWEEPING) {
      uint32_t el = millis() - tStart_;
      if (el >= sweepMsEff_) { complete(); return; }
      // Re-command the setpoint on a fixed cadence. It must stay SLOWER than the
      // poll interval: every control write defers the next poll (the device drops
      // commands that crowd each other), so updating faster than we poll would
      // starve telemetry entirely and the sweep would collect nothing.
      uint32_t step = setpointStepMs();
      if (lastSetMs_ == 0 || millis() - lastSetMs_ >= step) {
        lastSetMs_ = millis();
        currentTarget_ = clampToRatings(commandable(targetAt(el)));
        ble_->setSetpoint(currentTarget_);
      }
      return;
    }
    if (state_ == IDLE || timerCb_ == NONE) return;
    if ((int32_t)(millis() - timerAt_) < 0) return;
    TimerCb cb = timerCb_;
    timerCb_ = NONE;
    if (cb == PRIME_DONE) finishPriming();
  }

 private:
  enum State { IDLE, PRIMING, SWEEPING };
  enum TimerCb { NONE, PRIME_DONE };

  static const uint32_t PRIME_MS = 900;
  static constexpr float MIN_TEST_CURRENT = 0.05f;
  static constexpr float ABS_MAX_CURRENT = 40.0f;
  // Current bands for the reported curve. 32 is plenty to draw a V-I line and
  // to show curvature, and keeps the CSV readable.
  static const int NBINS = 32;

  struct Bin { double sumI = 0, sumV = 0, sumT = 0; int count = 0, fanMax = 0; };

  uint32_t primeMs() { return max(PRIME_MS, 2 * pollIntervalMs + 300); }

  // Lowest current the load will actually accept as a CC operating point. Below
  // this the EL15 reports the load off (see finishPriming), so the ramp floors
  // its commanded value here even when the user asked to start at 0 A. The
  // result reports the current range it MEASURED, so the floor is visible rather
  // than hidden.
  float commandable(float target) const { return max(target, MIN_TEST_CURRENT); }

  // One setpoint update per poll-and-a-bit. At the 20 Hz default this is 100 ms,
  // i.e. ~300 discrete setpoints across a 30 s sweep — 13 mA apart on a 4 A
  // ramp, which is smooth as far as the load is concerned.
  // The ramp re-commands the setpoint at a fixed 10 Hz, independent of the poll
  // rate. It used to be derived from the poll interval because every control
  // write cost a whole poll period; now that a command only holds the poll off
  // by CTRL_POLL_GAP_MS, the two cadences are genuinely independent — 10 Hz of
  // ramp AND the full 20 Hz of sampling. 100 ms is also comfortably above the
  // 50 ms the device needs between control writes.
  uint32_t setpointStepMs() const {
    return setpointMs > 0 ? setpointMs : RAMP_STEP_MS;
  }

  float elapsedS() const { return state_ == SWEEPING ? (millis() - tStart_) / 1000.0f : 0; }

  // Symmetric triangle: start -> max over the first half, back to start over the
  // second. Returns the commanded current at `elMs` into the sweep.
  float targetAt(uint32_t elMs) const {
    float half = sweepMsEff_ / 2.0f;
    if (half <= 0) return startCurrent_;
    float frac = elMs < half ? elMs / half : 2.0f - elMs / half;
    frac = constrain(frac, 0.0f, 1.0f);
    return startCurrent_ + (maxCurrent_ - startCurrent_) * frac;
  }

  void resetAccumulators() {
    n_ = 0; sumI_ = sumV_ = sumII_ = sumIV_ = sumVV_ = 0;
    minI_ = maxI_ = minV_ = maxV_ = 0;
    tempMin_ = tempMax_ = 0; haveTemp_ = false;
    fanMax_ = 0; peakW_ = 0;
    for (int k = 0; k < NBINS; k++) bins_[k] = Bin{};
    lastSetMs_ = 0; currentTarget_ = 0;
  }

  void binSample(const el15::Status &s) {
    float span = maxCurrent_ - startCurrent_;
    if (span <= 1e-6f) return;
    int k = (int)((s.current - startCurrent_) / span * NBINS);
    k = constrain(k, 0, NBINS - 1);
    Bin &b = bins_[k];
    b.sumI += s.current; b.sumV += s.voltage; b.sumT += s.temperature;
    b.count++;
    b.fanMax = max(b.fanMax, s.fanSpeed);
  }

  // Ordinary least squares on the running sums. False until the sweep has
  // covered enough current span to define a line.
  bool fitSlope(float &rOut, float *interceptOut = nullptr,
                float *r2Out = nullptr, float *stdErrOut = nullptr) const {
    if (n_ < 8) return false;
    double nn = (double)n_;
    double sII = sumII_ - sumI_ * sumI_ / nn;
    double sIV = sumIV_ - sumI_ * sumV_ / nn;
    double sVV = sumVV_ - sumV_ * sumV_ / nn;
    if (sII < 1e-9 || (maxI_ - minI_) < 0.05f) return false;
    double slope = sIV / sII;
    rOut = (float)max(-slope, 0.0);
    if (interceptOut) *interceptOut = (float)(sumV_ / nn - slope * (sumI_ / nn));
    if (r2Out) *r2Out = (sVV > 1e-9) ? (float)((sIV * sIV) / (sII * sVV)) : 0;
    if (stdErrOut) {
      // SSE = S_VV - slope*S_IV; Var(slope) = (SSE/(n-2)) / S_II.
      double sse = sVV - slope * sIV;
      if (sse < 0) sse = 0;
      *stdErrOut = (n_ > 2) ? (float)sqrt(sse / ((nn - 2) * sII)) : 0;
    }
    return true;
  }

  // 4-wire already excludes the leads; 2-wire subtracts the measured tare and
  // clamps at zero (a DUT below the tare means the tare is stale, not that
  // resistance went negative).
  float correct(float raw) const {
    return fourWire ? raw : max(raw - tareOhm, 0.0f);
  }

  float clampToRatings(float target) const {
    float v = vRecent_ > el15::MIN_VOLTAGE_V ? vRecent_ : el15::MAX_VOLTAGE_V;
    float powerCap = el15::MAX_POWER_W / v;
    return min(min(target, el15::MAX_CURRENT_A), min(powerCap, ABS_MAX_CURRENT));
  }

  void finishPriming() {
    float voc = vOcMeasured_;
    if (voc < el15::MIN_VOLTAGE_V) { abort_("No reading, or voltage below the minimum. Test aborted."); return; }
    if (voc > el15::MAX_VOLTAGE_V) { abort_("Source above the EL15's 60 V rating. Test aborted."); return; }

    float cap = safeMaxCurrent(fuseRating_, voc, safetyFactor);
    maxCurrent_ = (maxCurrent > 0) ? min(maxCurrent, cap) : cap;
    if (maxCurrent_ < MIN_TEST_CURRENT) { abort_("Safe test current too low - check the fuse rating."); return; }
    // The ramp's baseline is the lowest current the load will actually hold, NOT
    // zero. Interpolating from 0 and clipping on the way out left a flat,
    // commanded-but-unreachable segment at each end of the triangle — a
    // discontinuity in an otherwise smooth ramp, and dead time that collected
    // samples at a single current. Starting the interpolation AT the floor makes
    // the whole sweep linear and every sample useful.
    startCurrent_ = constrain(startCurrent, MIN_TEST_CURRENT,
                              max(maxCurrent_ - MIN_TEST_CURRENT, MIN_TEST_CURRENT));

    sweepMsEff_ = constrain(sweepSeconds, MIN_SWEEP_S, MAX_SWEEP_S) * 1000u;

    state_ = SWEEPING;
    tStart_ = millis();
    lastSetMs_ = millis();
    // Arm the re-assert timer so the FIRST check happens one interval into the
    // sweep, not one interval after some later event — a LOAD_ON that never
    // landed has to be caught at the start, where it costs the most.
    lastOnPushMs_ = millis();
    // Command a real current BEFORE switching the load on. Verified on hardware
    // 2026-08-01: the EL15 accepts LOAD_ON at a 0.000 A CC setpoint and then
    // simply reports the load off — the whole sweep ran with I=0 and the fit had
    // nothing to work with. The stepped ladder this replaced never hit it
    // because its first level was maxCurrent/n, never zero.
    currentTarget_ = clampToRatings(commandable(startCurrent_));
    ble_->setSetpoint(currentTarget_);
    ble_->setLoad(true);
  }

  void complete() {
    float r = 0, intercept = 0, r2 = 0, stdErr = 0;
    bool ok = fitSlope(r, &intercept, &r2, &stdErr);
    finishSafely();
    state_ = IDLE;
    if (!ok) {
      if (onError) onError("Not enough usable samples - raise the sample rate or lengthen the sweep.");
      return;
    }
    Result res;
    res.rawResistanceOhm = r;
    res.fourWire = fourWire;
    res.tareOhm = fourWire ? 0 : tareOhm;
    res.resistanceOhm = correct(r);
    res.openCircuitVoltage = intercept;
    res.rSquared = r2;
    res.resistanceStdErr = stdErr;
    res.fuseRating = fuseRating_;
    res.maxTestCurrent = maxCurrent_;
    res.startCurrent = startCurrent_;
    res.sweepSeconds = sweepMsEff_ / 1000;
    res.rawSamples = n_;
    res.minCurrent = minI_;
    res.maxCurrentSeen = maxI_;
    res.sagV = maxV_ - minV_;
    res.peakPowerW = peakW_;
    res.tempMin = haveTemp_ ? tempMin_ : 0;
    res.tempMax = haveTemp_ ? tempMax_ : 0;
    res.maxFan = fanMax_;
    res.loadDropouts = loadDropouts_;

    res.samples.reserve(NBINS);
    for (int k = 0; k < NBINS; k++) {
      const Bin &b = bins_[k];
      if (b.count == 0) continue;
      res.samples.push_back(Sample{(float)(b.sumI / b.count), (float)(b.sumV / b.count),
                                   (float)(b.sumT / b.count), b.fanMax});
    }

    // Reliable when the uncertainty is small in ABSOLUTE (<= 5 mohm) OR RELATIVE
    // (<= 5 %) terms — meaningful at any resistance scale, unlike R^2 which
    // false-alarms on clean low-milliohm reads and rubber-stamps noisy big ones.
    // Judge it against what was MEASURED, not the tare-corrected figure:
    // subtracting a constant cannot make the fit better, and a near-tare result
    // would otherwise look wildly unreliable.
    double relTol = res.rawResistanceOhm > 1e-4 ? stdErr / res.rawResistanceOhm : 1e9;
    res.reliable = n_ >= 20 && (maxI_ - minI_) > 0.05f &&
                   (stdErr <= 0.005 || relTol <= 0.05);
    if (onComplete) onComplete(res);
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
  static const uint32_t RAMP_STEP_MS = 100;      // 10 Hz setpoint updates
  // How soon a load reported off is re-asserted. Measured on hardware: the
  // INITIAL LOAD_ON does not always land, and at the old 1 s interval that lost
  // the first second of the sweep — the whole low-current end of the ramp, which
  // is where the fit most needs span (one such run reported a minimum current of
  // 0.07 A instead of 0.05 and a 1.5% worse spread). 400 ms catches it while
  // still being far longer than the 50 ms the device needs between commands.
  static const uint32_t LOAD_REASSERT_MS = 400;
  static const uint32_t FAULT_CLEAR_MS = 4000;   // grace period to shed a stale trip

  uint32_t timerAt_ = 0, tStart_ = 0, lastSetMs_ = 0, lastOnPushMs_ = 0;
  uint32_t primeStartMs_ = 0, lastClearPushMs_ = 0;
  uint32_t sweepMsEff_ = 30000;
  int loadDropouts_ = 0;   // how often the load had to be re-asserted

  // Running regression sums — the whole sample history in six doubles.
  uint32_t n_ = 0;
  double sumI_ = 0, sumV_ = 0, sumII_ = 0, sumIV_ = 0, sumVV_ = 0;
  float minI_ = 0, maxI_ = 0, minV_ = 0, maxV_ = 0;
  float tempMin_ = 0, tempMax_ = 0;
  bool haveTemp_ = false;
  int fanMax_ = 0;
  float peakW_ = 0;

  Bin bins_[NBINS];

  float fuseRating_ = 0, maxCurrent_ = 0, startCurrent_ = 0, currentTarget_ = 0;
  float vOcMeasured_ = 0, vRecent_ = 0;
};
