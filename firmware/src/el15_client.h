// BLE central client for the EL15 load — the ESP32 counterpart of
// El15BleManager. Scans for the FFF0 service, connects, subscribes to FFF1
// status notifications, writes command frames to FFF3, and polls at a fixed
// rate. Reassembles status frames chunked across notifications, exactly like
// the Android manager.
//
// Threading: NimBLE runs its host in its own FreeRTOS task, so the scan,
// connect/disconnect, and notification callbacks fire OFF the Arduino loop
// task. They must not touch LVGL or the resistance-test engine directly (LVGL
// is not reentrant, and the test engine's buffers are pumped from loop()).
// Instead each callback marshals a small event onto evtQueue_; drainEvents(),
// called from loopTick() on the loop task, dispatches to onState/onStatus/
// onDeviceFound. So every downstream consumer runs single-threaded on loop().
#pragma once

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <functional>
#include <vector>
#include "el15_controller.h"

class El15Client : public El15Controller {
 public:
  enum State { IDLE, SCANNING, CONNECTING, CONNECTED };

  // Callbacks are dispatched from drainEvents()/loopTick() on the loop task.
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

  // Pump periodic work: drain queued BLE events, then poll. Call from loop().
  void loopTick();

  // Invoked by NimBLE callbacks on the HOST task — these ONLY enqueue.
  void enqueueNotify(const uint8_t *data, size_t len);
  void enqueueDeviceFound(const NimBLEAdvertisedDevice *dev);
  void enqueueDisconnected();

 private:
  // A BLE event marshalled from the NimBLE host task to the loop task.
  struct Event {
    enum Kind : uint8_t { NOTIFY, DISCONNECTED, DEVICE_FOUND } kind;
    uint8_t len;              // NOTIFY: payload length
    uint8_t addrType;         // DEVICE_FOUND: BLE address type (public/random)
    uint8_t data[64];         // NOTIFY: raw notification bytes
    char addr[24];            // DEVICE_FOUND
    char name[40];            // DEVICE_FOUND
  };

  void drainEvents();                                 // loop task
  void handleNotify(const uint8_t *data, size_t len); // loop task (reassembly)
  void handleDisconnect();                            // loop task

  void setState(State s, const char *info);
  void write(const el15::Frame &f) { writeRaw(f.data(), f.size()); }
  void writeFixed(const uint8_t *d, size_t n) { writeRaw(d, n); }
  void writeRaw(const uint8_t *d, size_t n);
  void poll();

  State state_ = IDLE;
  NimBLEClient *client_ = nullptr;
  NimBLERemoteCharacteristic *writeChar_ = nullptr;
  NimBLERemoteCharacteristic *notifyChar_ = nullptr;

  QueueHandle_t evtQueue_ = nullptr;

  // Addresses seen during the current scan, WITH their real type (public vs
  // random). connectTo() reuses the discovered type instead of forcing public,
  // so random-address peers (phones, some EL15 units) connect. Loop task only.
  std::vector<NimBLEAddress> scanAddrs_;

  // Frame reassembly buffer for status packets split across notifications.
  uint8_t frameBuf_[64];
  size_t frameLen_ = 0;

  uint32_t lastPollMs_ = 0;
};
