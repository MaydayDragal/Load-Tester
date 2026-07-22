// Abstract load controller — the resistance-test engine and UI talk to this,
// not to BLE directly, so a built-in simulator can stand in for real hardware
// (mirrors the El15Controller interface + El15Simulator in the Android app).
#pragma once

#include "el15_protocol.h"

class El15Controller {
 public:
  virtual ~El15Controller() {}
  virtual void setMode(int mode) = 0;
  virtual void setSetpoint(float value) = 0;
  virtual void setLoad(bool on) = 0;
  virtual void setLock() = 0;
};

// A fake load modelling an ideal source (emf) behind a series resistance, so
// the whole firmware — live readouts, manual control, the resistance test —
// can be exercised without hardware. A resistance test against it recovers
// ~seriesR ohms and ~emf volts.
class El15Simulator : public El15Controller {
 public:
  float emf = 12.6f;
  float seriesR = 0.35f;

  void setMode(int m) override {
    mode_ = m;
    energyWh_ = 0; capacityAh_ = 0; runtimeSec_ = 0; runMsAcc_ = 0;
  }
  void setSetpoint(float v) override { setpoint_ = v < 0 ? 0 : v; }
  void setLoad(bool on) override { loadOn_ = on; if (!on) { runtimeSec_ = 0; runMsAcc_ = 0; } }
  void setLock() override { lockOn_ = !lockOn_; }

  // Battery emulation for the demo capacity test: while active, the source emf
  // sags as charge is drawn — a li-ion-ish plateau with a fast tail, ending
  // ~40 % below full so any sane cutoff voltage is crossed near empty.
  void battStart(float ah) { battAh_ = ah; battDrawn_ = 0; }
  void battStop() { battAh_ = 0; }

  // Produce a status snapshot for the current state; call at the poll rate.
  el15::Status tick(uint32_t dtMs) {
    float rEff = seriesR < 0.001f ? 0.001f : seriesR;
    float e = emf;
    if (battAh_ > 0) {
      float soc = 1.0f - battDrawn_ / battAh_;
      if (soc < 0) soc = 0;
      e = emf * (0.60f + 0.40f * powf(soc, 0.25f));
    }
    float current = 0, voltage = e;
    if (loadOn_) {
      float iCeil = (e - 0.3f) / rEff;
      switch (mode_) {
        case el15::MODE_CV: {
          float vSet = setpoint_ < e ? setpoint_ : e;
          current = (e - vSet) / rEff; if (current < 0) current = 0;
          break;
        }
        case el15::MODE_CR: {
          float rLoad = setpoint_ < 0.01f ? 0.01f : setpoint_;
          current = e / (rEff + rLoad);
          break;
        }
        case el15::MODE_CP: {
          float disc = e * e - 4 * rEff * setpoint_;
          current = disc <= 0 ? iCeil : min((e - sqrtf(disc)) / (2 * rEff), iCeil);
          break;
        }
        default:  // CC / CAP / DCR
          current = min(setpoint_, iCeil);
          break;
      }
      if (current < 0) current = 0;
      if (current > el15::MAX_CURRENT_A) current = el15::MAX_CURRENT_A;
      voltage = e - current * rEff; if (voltage < 0) voltage = 0;
      runMsAcc_ += dtMs;
      runtimeSec_ = runMsAcc_ / 1000;
      float dtH = dtMs / 3600000.0f;
      energyWh_ += voltage * current * dtH;
      capacityAh_ += current * dtH;
      if (battAh_ > 0) battDrawn_ += current * dtH;
    }

    el15::Status s;
    s.valid = true; s.crcPass = true;
    s.mode = mode_; s.modeStr = el15::modeName(mode_);
    s.voltage = withNoise(voltage); s.current = withNoise(current);
    s.power = s.voltage * s.current;
    s.loadOn = loadOn_; s.lockOn = lockOn_;
    s.runtime = runtimeSec_;
    s.temperature = 24.5f + current * 0.9f;
    s.fanSpeed = current > 8 ? el15::FAN_SPEED_MAX : current > 4 ? 3 : current > 1 ? 1 : 0;
    s.ready = loadOn_;
    el15::SetpointInfo info = el15::setpointInfo(mode_);
    s.setpointUnit = info.unit; s.setpointDecimals = info.decimals; s.setpointLabel = info.label;
    if (mode_ == el15::MODE_CAP) { s.energyWh = energyWh_; s.capacityAh = capacityAh_; s.setpointInPacket = false; }
    else if (mode_ == el15::MODE_DCR) { s.dcrMilliOhm = rEff * 1000; s.dcrI1 = current; s.dcrI2 = current * 0.5f; s.current = 0; s.power = 0; s.setpointInPacket = false; }
    else { s.setpoint = setpoint_; s.setpointInPacket = true; }
    return s;
  }

 private:
  int mode_ = el15::MODE_CC;
  float setpoint_ = 0;
  bool loadOn_ = false, lockOn_ = false;
  int runtimeSec_ = 0;
  uint32_t runMsAcc_ = 0;
  float energyWh_ = 0, capacityAh_ = 0;
  float battAh_ = 0, battDrawn_ = 0;
  uint32_t rng_ = 0x2545F491;

  float withNoise(float v) {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    float unit = ((int)(rng_ % 1000) / 1000.0f) * 2.0f - 1.0f;
    return v * (1.0f + unit * 0.0015f);
  }
};
