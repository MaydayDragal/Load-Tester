// BLE central client for the EL15 load — the ESP32 counterpart of
// El15BleManager. Scans for the FFF0 service, connects, subscribes to FFF1
// status notifications, writes command frames to FFF3, and polls at a fixed
// rate. Reassembles status frames chunked across notifications, exactly like
// the Android manager.
#pragma once

#include <NimBLEDevice.h>
#include <functional>
#include "el15_controller.h"

class El15Client : public El15Controller {
 public:
  enum State { IDLE, SCANNING, CONNECTING, CONNECTED };

  // Callbacks are invoked from the NimBLE/loop context on the main task.
  std::function<void(State, const char *)> onState;
  std::function<void(const el15::Status &)> onStatus;
  // Reports each discovered EL15 candidate during a scan (address, name).
  std::function<void(const char *, const char *)> onDeviceFound;

  uint32_t pollIntervalMs = 500;

  void begin();
  void startScan(uint32_t seconds = 8);
  void stopScan();
  bool connectTo(const char *address);   // quick-reconnect by MAC
  void disconnect();
  void shutdownAndDisconnect();          // write LOAD_OFF, then drop the link
  State state() const { return state_; }

  // El15Controller — command frames identical to the Android app.
  void setMode(int mode) override { write(el15::modeCommand(mode)); }
  void setSetpoint(float v) override { write(el15::setpointCommand(v)); }
  void setLoad(bool on) override { writeFixed(on ? el15::LOAD_ON : el15::LOAD_OFF,
                                              on ? sizeof(el15::LOAD_ON) : sizeof(el15::LOAD_OFF)); }
  void setLock() override { writeFixed(el15::LOCK, sizeof(el15::LOCK)); }

  // Pump periodic work (polling). Call from loop().
  void loopTick();

  // Internal — invoked by NimBLE callbacks (public so the callback structs reach them).
  void handleNotify(const uint8_t *data, size_t len);
  void handleConnect(bool ok);
  void handleDisconnect();
  void handleScanResult(NimBLEAdvertisedDevice *dev);

 private:
  void setState(State s, const char *info);
  void write(const el15::Frame &f) { writeRaw(f.data(), f.size()); }
  void writeFixed(const uint8_t *d, size_t n) { writeRaw(d, n); }
  void writeRaw(const uint8_t *d, size_t n);
  void poll();

  State state_ = IDLE;
  NimBLEClient *client_ = nullptr;
  NimBLERemoteCharacteristic *writeChar_ = nullptr;
  NimBLERemoteCharacteristic *notifyChar_ = nullptr;

  // Frame reassembly buffer for status packets split across notifications.
  uint8_t frameBuf_[64];
  size_t frameLen_ = 0;

  uint32_t lastPollMs_ = 0;
};
