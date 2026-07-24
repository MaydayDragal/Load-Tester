#include "el15_client.h"

// NimBLE-Arduino 2.x. If you are on 1.x, the callback signatures differ
// slightly (NimBLEAdvertisedDevice* vs const&, connect/onResult prototypes) -
// see the README's version note.
//
// All NimBLE callbacks below run on the NimBLE HOST task. They do the minimum
// possible - copy the payload and push it onto El15Client's event queue - and
// let drainEvents() (loop task) do the parsing and fan-out. Nothing here may
// touch LVGL or the resistance-test engine.

static El15Client *g_self = nullptr;

#ifdef EL15_POLLTEST
// Poll-rate sweep: at each interval, poll as commanded and compare each received
// 28-byte status frame to the previous one. A frame that differs is FRESH data;
// an identical frame is a wasted poll (the device hasn't re-sampled yet). As the
// poll rate climbs past the device's true update rate, rx Hz keeps rising but
// fresh Hz plateaus and the unique-% falls — the plateau is the max useful rate.
namespace {
const uint32_t PT_INTERVALS[] = {250, 200, 150, 100, 75, 50, 33, 25, 20};
const int PT_N = (int)(sizeof(PT_INTERVALS) / sizeof(PT_INTERVALS[0]));
const uint32_t PT_WINDOW_MS = 3500;
int pt_idx = -1;
bool pt_done = false;
uint32_t pt_winStart = 0, pt_rx = 0, pt_changed = 0, pt_lastChangeMs = 0, pt_minGap = 0xFFFFFFFF;
uint8_t pt_prev[28];
bool pt_havePrev = false;
void pt_resetWindow(uint32_t now) {
  pt_winStart = now; pt_rx = 0; pt_changed = 0; pt_lastChangeMs = 0;
  pt_minGap = 0xFFFFFFFF; pt_havePrev = false;
}
}  // namespace
#endif

// ---- Scan callback ---------------------------------------------------------
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    if (g_self) g_self->enqueueDeviceFound(dev);
  }
};
static ScanCallbacks g_scanCallbacks;

// ---- Client (connection) callback -----------------------------------------
class ClientCallbacks : public NimBLEClientCallbacks {
  // Service discovery is done synchronously in connectTo() on the loop task, so
  // onConnect has nothing to do here (running a blocking GATT discovery from
  // this host-task callback can wedge the NimBLE host).
  void onConnect(NimBLEClient *) override {}
  void onDisconnect(NimBLEClient *, int) override { if (g_self) g_self->enqueueDisconnected(); }
};
static ClientCallbacks g_clientCallbacks;

// ---- Notification trampoline ----------------------------------------------
static void notifyCb(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  if (g_self) g_self->enqueueNotify(data, len);
}

void El15Client::begin() {
  g_self = this;
  evtQueue_ = xQueueCreate(16, sizeof(Event));
  NimBLEDevice::init("EL15-Controller");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  // Request a larger ATT MTU so a 28-byte status frame arrives in a single
  // notification. At the default 23-byte MTU the payload caps at 20 bytes and
  // the frame would be truncated (the peripheral notify isn't auto-fragmented).
  NimBLEDevice::setMTU(247);
}

void El15Client::setState(State s, const char *info) {
  state_ = s;
  if (onState) onState(s, info);
}

// ---- Event queue (host task -> loop task) ---------------------------------
void El15Client::enqueueNotify(const uint8_t *data, size_t len) {
  if (!evtQueue_) return;
  Event e;
  e.kind = Event::NOTIFY;
  if (len > sizeof(e.data)) len = sizeof(e.data);
  e.len = (uint8_t)len;
  memcpy(e.data, data, len);
  xQueueSend(evtQueue_, &e, 0);  // drop if full; the next poll refreshes state
}

void El15Client::enqueueDeviceFound(const NimBLEAdvertisedDevice *dev) {
  if (!evtQueue_) return;
  // The EL15 often omits FFF0 from its advert, so surface every *named* device
  // and verify the service after connecting (like the Android scan).
  std::string name = dev->getName();
  if (name.empty()) return;
  Event e;
  e.kind = Event::DEVICE_FOUND;
  NimBLEAddress a = dev->getAddress();
  e.addrType = a.getType();
  std::string addr = a.toString();
  snprintf(e.addr, sizeof(e.addr), "%s", addr.c_str());
  snprintf(e.name, sizeof(e.name), "%s", name.c_str());
  xQueueSend(evtQueue_, &e, 0);  // dropping a duplicate advert is harmless
}

void El15Client::enqueueDisconnected() {
  if (!evtQueue_) return;
  Event e;
  e.kind = Event::DISCONNECTED;
  // Disconnect is rare and must not be lost, so allow a brief wait if the queue
  // is momentarily full.
  xQueueSend(evtQueue_, &e, pdMS_TO_TICKS(20));
}

void El15Client::drainEvents() {
  if (!evtQueue_) return;
  Event e;
  while (xQueueReceive(evtQueue_, &e, 0) == pdTRUE) {
    switch (e.kind) {
      case Event::NOTIFY:       handleNotify(e.data, e.len); break;
      case Event::DISCONNECTED: handleDisconnect(); break;
      case Event::DEVICE_FOUND: {
        // Remember the address WITH its type, and surface each address only once
        // (advertisements repeat many times per second).
        NimBLEAddress a(std::string(e.addr), e.addrType);
        bool known = false;
        for (auto &x : scanAddrs_) if (x == a) { known = true; break; }
        if (!known) {
          scanAddrs_.push_back(a);
          if (onDeviceFound) onDeviceFound(e.addr, e.name);
        }
        break;
      }
    }
  }
}

// ---- Scanning --------------------------------------------------------------
void El15Client::startScan(uint32_t seconds) {
  scanAddrs_.clear();
  setState(SCANNING, "Scanning...");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&g_scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(30);
  scan->start(seconds * 1000, false);
}

void El15Client::stopScan() {
  NimBLEDevice::getScan()->stop();
  if (state_ == SCANNING) setState(IDLE, "Idle");
}

// ---- Connection ------------------------------------------------------------
bool El15Client::connectTo(const char *address) {
  // Prefer the EXACT NimBLEAddress captured during the scan. For a phone's
  // resolvable private address the scanned object is the peer as the controller
  // actually saw and (if privacy is on) resolved it; a string round-trip can
  // lose that, and connecting to a bare RPA that has since rotated gives the
  // HCI 0x3e "connection failed to be established" we were chasing. Fall back to
  // reconstructing only for an address we never scanned (the guard's stored
  // peer, via the overload below).
  for (auto &x : scanAddrs_)
    if (x.toString() == std::string(address)) return connectAddr(x);
  return connectAddr(NimBLEAddress(std::string(address), BLE_ADDR_PUBLIC));
}

bool El15Client::connectTo(const char *address, int addrType) {
  return connectAddr(NimBLEAddress(std::string(address), (uint8_t)addrType));
}

bool El15Client::connectAddr(const NimBLEAddress &addr) {
  stopScan();
  setState(CONNECTING, "Connecting...");
  if (!client_) {
    client_ = NimBLEDevice::createClient();
    client_->setClientCallbacks(&g_clientCallbacks, false);
  }
  client_->setConnectTimeout(connectTimeoutMs_);
  // Connecting to an Android RPA peripheral (the simulator) often fails the
  // first connection establishment with HCI 0x3e — the link half-forms but the
  // central misses the early connection events. Each retry sends a FRESH
  // connection request, so several short attempts catch it where one does not.
  // NimBLE blocks this call for timeout x (retries + 1); with a 4 s timeout and
  // 4 retries that is ~20 s worst case on a truly dead peer (a healthy one
  // connects on the first attempt in well under a second). connectFailRetries
  // only re-tries the 0x3e establishment failure, which is exactly ours.
  NimBLEClient::Config cfg = client_->getConfig();
  cfg.connectFailRetries = connectRetries_;
  client_->setConfig(cfg);
  Serial.printf("[ble] connecting to %s (addr type %d)\n",
                addr.toString().c_str(), addr.getType());
  if (!client_->connect(addr)) {
    // rc 13 = BLE_HS_ETIMEOUT / HCI 0x3e: the peer never completed the
    // handshake (out of range, rotated its address, or an Android RPA peripheral
    // not accepting). Keep the client for reuse — deleting it here races the
    // controller's late disconnect event ("client not found").
    Serial.printf("[ble] connect() FAILED rc=%d\n", client_->getLastError());
    setState(IDLE, "Connect failed");
    return false;
  }
  // Connected. Resolve the FFF0 service and its notify/write characteristics
  // here on the loop task - never from the host-task onConnect callback.
  NimBLERemoteService *svc = client_->getService(el15::SERVICE_UUID);
  if (!svc) { disconnect(); setState(IDLE, "Not an EL15 (no FFF0)"); return false; }
  notifyChar_ = svc->getCharacteristic(el15::NOTIFY_UUID);
  writeChar_ = svc->getCharacteristic(el15::WRITE_UUID);
  if (!notifyChar_ || !writeChar_) { disconnect(); setState(IDLE, "EL15 characteristics missing"); return false; }
  if (notifyChar_->canNotify()) notifyChar_->subscribe(true, notifyCb);

  // Forensic GATT dump. A real EL15 that differs from the reverse-engineered
  // reference capture — a different command characteristic, or a write char that
  // only accepts write-WITH-response — otherwise presents as "connected, live
  // telemetry, but every command ignored", which is exactly what we hit on first
  // real hardware. Flags: R=read W=write(with-response) w=write-no-response
  // N=notify I=indicate. If FFF3 shows "W" but not "w", the fix is to write with
  // response (writeRaw does this automatically now).
  // getCharacteristics(false): return the ALREADY-discovered characteristics.
  // Passing true forces a re-discovery that frees and recreates every
  // characteristic object — invalidating notifyChar_/writeChar_ resolved just
  // above, so the next poll dereferences freed memory and panics on connect.
  Serial.println("[ble] FFF0 characteristics:");
  for (auto *c : svc->getCharacteristics(false)) {
    Serial.printf("[ble]   %s h=%u %s%s%s%s%s\n",
                  c->getUUID().toString().c_str(), c->getHandle(),
                  c->canRead() ? "R" : "-",
                  c->canWrite() ? "W" : "-",
                  c->canWriteNoResponse() ? "w" : "-",
                  c->canNotify() ? "N" : "-",
                  c->canIndicate() ? "I" : "-");
  }
  Serial.printf("[ble] command char FFF3 supports: write-req=%d write-cmd=%d\n",
                (int)writeChar_->canWrite(), (int)writeChar_->canWriteNoResponse());

  frameLen_ = 0;
  lastPollMs_ = 0;
  snprintf(lastAddr_, sizeof(lastAddr_), "%s", addr.toString().c_str());
  lastAddrType_ = addr.getType();
  setState(CONNECTED, "Connected - FFF0");
  return true;
}

void El15Client::handleDisconnect() {
  notifyChar_ = nullptr;
  writeChar_ = nullptr;
  frameLen_ = 0;
  setState(IDLE, "Disconnected");
}

void El15Client::disconnect() {
  if (client_ && client_->isConnected()) client_->disconnect();
  else handleDisconnect();
}

void El15Client::shutdownAndDisconnect() {
  if (state_ == CONNECTED && writeChar_) {
    writeFixed(el15::LOAD_OFF, sizeof(el15::LOAD_OFF));
    delay(40);  // let the write flush before tearing the link down
  }
  disconnect();
}

// ---- I/O -------------------------------------------------------------------
void El15Client::writeRaw(const uint8_t *d, size_t n) {
  if (state_ != CONNECTED || !writeChar_) return;

  // FFF3 is write-no-response only, and the device silently DROPS a no-response
  // write that arrives too close behind another one (proven on hardware: mode
  // changes and the sweep's LOAD_ON-after-setpoint were being lost). So pace
  // every CONTROL command (frame byte 3 != 0x08; the poll is 0x08) at least
  // CTRL_GAP_MS apart from the previous control write AND clear of the next poll.
  // Polls themselves are never paced — they self-retry every interval.
  const bool isCtrl = (n >= 4 && d[3] != 0x08);
  if (isCtrl) {
    static const uint32_t CTRL_GAP_MS = 50;   // > one BLE connection interval
    static uint32_t lastCtrlMs = 0;
    uint32_t since = millis() - lastCtrlMs;
    if (lastCtrlMs && since < CTRL_GAP_MS) delay(CTRL_GAP_MS - since);
    lastCtrlMs = millis();
  }

  bool withResp = writeChar_->canWrite();
  bool ok = writeChar_->writeValue(const_cast<uint8_t *>(d), n, withResp);
  if (isCtrl) lastPollMs_ = millis();   // keep the next poll off this command

  // Log control writes, but not the twice-a-second poll, so the serial log stays
  // readable while still showing every mode/setpoint/load command.
  if (isCtrl) {
    Serial.printf("[ble] write %s (%s):", ok ? "OK" : "FAIL",
                  withResp ? "with-resp" : "no-resp");
    for (size_t i = 0; i < n; i++) Serial.printf(" %02X", d[i]);
    Serial.println();
  }
}

void El15Client::handleNotify(const uint8_t *data, size_t len) {
  // Reassemble: append, then parse whenever a header-aligned 28-byte frame is
  // buffered. Resync to the most recent header if bytes are lost.
  if (frameLen_ + len > sizeof(frameBuf_)) frameLen_ = 0;
  memcpy(frameBuf_ + frameLen_, data, len);
  frameLen_ += len;

  while (frameLen_ >= 28) {
    if (!el15::isStatusPacket(frameBuf_, frameLen_)) {
      // Drop one byte and try to resync on the next header.
      memmove(frameBuf_, frameBuf_ + 1, --frameLen_);
      continue;
    }
    el15::Status s = el15::parseStatus(frameBuf_, 28);
    if (!s.valid) {
      // A header-aligned frame that fails the checksum is dropped SILENTLY by
      // the parser — against an untested real device, a checksum-convention
      // mismatch would otherwise present as "connected, but Monitor blank"
      // with a clean serial log. Rate-limited so a noisy link can't spam.
      static uint32_t lastDropLogMs = 0;
      uint32_t now = millis();
      if (now - lastDropLogMs > 2000) {
        lastDropLogMs = now;
        Serial.printf("[ble] status frame DROPPED (checksum): %02X %02X %02X %02X %02X %02X %02X %02X ... sum&0xFF=%02X\n",
                      frameBuf_[0], frameBuf_[1], frameBuf_[2], frameBuf_[3],
                      frameBuf_[4], frameBuf_[5], frameBuf_[6], frameBuf_[7],
                      [&] { int sum = 0; for (int i = 0; i < 28; i++) sum += frameBuf_[i]; return sum & 0xFF; }());
      }
    }
    // Rate-limited proof-of-life for received status. Used to answer "does
    // telemetry keep flowing when we stop polling?" — if these lines continue
    // with polling disabled, the device free-runs status and our FFF3 writes are
    // unproven; if they stop, POLL (a FFF3 write) drives them and writes work.
    if (s.valid) {
      static uint32_t lastRxLogMs = 0;
      uint32_t nowRx = millis();
      if (nowRx - lastRxLogMs > 1000) {
        lastRxLogMs = nowRx;
        Serial.printf("[ble] status rx: V=%.2f I=%.3f mode=%s(0x%02X) load=%d  raw b5=0x%02X b6=0x%02X\n",
                      s.voltage, s.current, s.modeStr, s.mode, (int)s.loadOn,
                      frameBuf_[5], frameBuf_[6]);
      }
    }
#ifdef EL15_POLLTEST
    if (s.valid && pt_idx >= 0 && !pt_done) {
      pt_rx++;
      bool changed = !pt_havePrev || memcmp(pt_prev, frameBuf_, 28) != 0;
      if (changed) {
        pt_changed++;
        uint32_t nowc = millis();
        if (pt_lastChangeMs) { uint32_t g = nowc - pt_lastChangeMs; if (g < pt_minGap) pt_minGap = g; }
        pt_lastChangeMs = nowc;
      }
      memcpy(pt_prev, frameBuf_, 28);
      pt_havePrev = true;
    }
#endif
    if (onStatus) onStatus(s);
    memmove(frameBuf_, frameBuf_ + 28, frameLen_ - 28);
    frameLen_ -= 28;
  }
}

void El15Client::poll() { writeFixed(el15::POLL, sizeof(el15::POLL)); }

void El15Client::loopTick() {
  drainEvents();  // process queued BLE events on THIS (loop) task
  if (state_ != CONNECTED) return;

#ifdef EL15_POLLTEST
  if (!pt_done) {
    uint32_t nowp = millis();
    if (pt_idx < 0) {
      pt_idx = 0; pt_resetWindow(nowp); pollIntervalMs = PT_INTERVALS[0];
      Serial.println("[polltest] sweeping poll intervals; comparing 28-byte frames (fresh vs repeated)");
    } else if (nowp - pt_winStart >= PT_WINDOW_MS) {
      float sec = (nowp - pt_winStart) / 1000.0f;
      float rxHz = pt_rx / sec, freshHz = pt_changed / sec;
      int pct = pt_rx ? (int)(100.0f * pt_changed / pt_rx) : 0;
      Serial.printf("[polltest] poll=%3ums | rx=%5.1f Hz | fresh=%5.1f Hz (%3d%% unique) | fastest fresh gap=%lums\n",
                    (unsigned)PT_INTERVALS[pt_idx], rxHz, freshHz, pct,
                    (unsigned long)(pt_minGap == 0xFFFFFFFF ? 0 : pt_minGap));
      pt_idx++;
      if (pt_idx >= PT_N) {
        pt_done = true; pollIntervalMs = 500;
        Serial.println("[polltest] DONE (poll restored to 500 ms)");
      } else {
        pt_resetWindow(nowp); pollIntervalMs = PT_INTERVALS[pt_idx];
      }
    }
  }
#endif

#ifdef EL15_SELFTEST
  // SAFE mode sweep: mode changes draw no current (load stays OFF), so this
  // cannot energise anything. Step through every selectable mode ~2.5 s apart
  // and command it; the "status rx: ... mode=" line then shows what the device
  // actually switched to. Any commanded->reported mismatch is a bad SET opcode.
  static const struct { int id; const char *name; } SWEEP[] = {
    {el15::MODE_CC, "CC"}, {el15::MODE_CV, "CV"}, {el15::MODE_CC, "CC"},
    {el15::MODE_CV, "CV"}, {el15::MODE_CR, "CR"}, {el15::MODE_CP, "CP"},
    {el15::MODE_CAP, "CAP"}, {el15::MODE_DCR, "DCR"}, {el15::MODE_CC, "CC (restore)"},
  };
  static uint32_t connMs = 0;
  static int stStage = -1;
  if (connMs == 0) { connMs = millis(); stStage = -1; }
  uint32_t stEl = millis() - connMs;
  int want = (int)(stEl / 2500) - 1;   // stress at the previously-failing 2.5 s
  if (want > stStage && want >= 0 && want < (int)(sizeof(SWEEP) / sizeof(SWEEP[0]))) {
    stStage = want;
    // Mirror the R-test start sequence — a setpoint write IMMEDIATELY followed by
    // a second control write (here a mode change; safe, draws no current). If the
    // second command lands (mode follows), the same pacing makes the sweep's
    // LOAD_ON-after-setpoint land too. Load stays OFF throughout.
    Serial.printf("[selftest] back-to-back setpoint(0) + MODE -> %s (0x%02X)\n",
                  SWEEP[want].name, SWEEP[want].id);
    write(el15::setpointCommand(0.0f));
    write(el15::modeCommand(SWEEP[want].id));
  }
#endif

#ifndef EL15_NO_POLL
  uint32_t now = millis();
  if (now - lastPollMs_ >= pollIntervalMs) {
    lastPollMs_ = now;
    poll();
  }
#endif
}
