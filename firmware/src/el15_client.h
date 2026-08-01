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
  // Minimum separation between a control write and the next poll. Two CONTROL
  // writes still need CTRL_GAP_MS (50 ms) between them — that is the rule the
  // device actually enforces by dropping the second — but a poll is a different
  // opcode and needs less room. Smaller = more samples during a sweep; too
  // small and the device answers a poll that crowds a setpoint change with a
  // reading that has not caught up, which shows up as scatter in the fit.
  // 25 ms measured best on real hardware (3-way A/B, 0.5 A / 10 s sweeps):
  // 150 samples per 10 s against 87-95 at 60 ms, for the SAME fitted
  // uncertainty. The extra samples come with proportionally noisier individual
  // points (R2 0.979 vs 0.987), so sigma_R comes out a wash — but more points
  // is the more robust place to be, and it is what makes a 20 Hz sample rate
  // mean something during a sweep.
  uint32_t ctrlPollGapMs = 25;

  void begin();
  void startScan(uint32_t seconds = 8);
  void stopScan();
  bool connectTo(const char *address);   // quick-reconnect by MAC
  // Reconnect to an address that was NOT seen in this session's scan (a stored
  // one, after a reboot or a link loss). The address type must be supplied
  // because it can't be recovered from the string, and a random-address peer is
  // unreachable if we guess "public".
  bool connectTo(const char *address, int addrType);
  void disconnect();
  void shutdownAndDisconnect();          // write LOAD_OFF, then drop the link
  State state() const { return state_; }

  // The peer of the last successful connect, so a supervisor can get back to it
  // without a scan. Empty until one succeeds.
  const char *lastAddress() const { return lastAddr_; }
  int lastAddressType() const { return lastAddrType_; }

  // Per-attempt connect timeout and how many extra 0x3e establishment retries
  // connectTo() makes. The guard saves and restores both around its own attempts
  // (it has its own outer retry loop, so it wants 0 inner retries and a short
  // timeout) — hence the getters.
  void setConnectTimeoutMs(uint32_t ms) { connectTimeoutMs_ = ms; }
  uint32_t connectTimeoutMs() const { return connectTimeoutMs_; }
  void setConnectRetries(uint8_t n) { connectRetries_ = n; }
  uint8_t connectRetries() const { return connectRetries_; }

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

  bool connectAddr(const NimBLEAddress &addr);   // shared connect body
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
  // Earliest a poll may follow a control write. A control write used to reset
  // lastPollMs_ outright, which cost a WHOLE poll interval every time — during
  // a resistance sweep, which re-commands the setpoint at 10 Hz, that halved the
  // sample rate (a 20 Hz poll delivered ~12 Hz of data). Holding the poll off by
  // a short fixed gap instead keeps commands from being crowded without
  // sacrificing the poll cadence the user asked for.
  uint32_t pollHoldUntilMs_ = 0;

  char lastAddr_[24] = "";
  int lastAddrType_ = 0;
  // Per-attempt connect timeout + extra establishment retries. A present peer
  // connects in well under a second; 4 s x (4 + 1) attempts catches a flaky RPA
  // establishment while bounding a dead-peer connect to ~20 s.
  uint32_t connectTimeoutMs_ = 4000;
  uint8_t connectRetries_ = 4;
};
