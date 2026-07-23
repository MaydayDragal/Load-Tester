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
  // WRITE_NO_RESPONSE mirrors the Android write type; ignore transient failures.
  writeChar_->writeValue(const_cast<uint8_t *>(d), n, false);
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
    if (onStatus) onStatus(s);
    memmove(frameBuf_, frameBuf_ + 28, frameLen_ - 28);
    frameLen_ -= 28;
  }
}

void El15Client::poll() { writeFixed(el15::POLL, sizeof(el15::POLL)); }

void El15Client::loopTick() {
  drainEvents();  // process queued BLE events on THIS (loop) task
  if (state_ != CONNECTED) return;
  uint32_t now = millis();
  if (now - lastPollMs_ >= pollIntervalMs) {
    lastPollMs_ = now;
    poll();
  }
}
