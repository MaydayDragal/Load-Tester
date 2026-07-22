// Abstract load controller — the resistance-test engine and UI talk to this,
// not to BLE directly. On this device the only implementation is El15Client
// (the BLE central): simulation is deliberately NOT done on the ESP32. To test
// without real load hardware, run the Android "EL15 Load Simulator" app
// (simulator/ in this repo), which impersonates the load over a genuine BLE
// link — the firmware then exercises its real transport path end to end.
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
