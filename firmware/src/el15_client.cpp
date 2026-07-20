#include "el15_client.h"

// NimBLE-Arduino 2.x. If you are on 1.x, the callback signatures differ
// slightly (NimBLEAdvertisedDevice* vs const&, connect/onResult prototypes) —
// see the README's version note.

static El15Client *g_self = nullptr;

// ---- Scan callback ---------------------------------------------------------
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    if (g_self) g_self->handleScanResult(const_cast<NimBLEAdvertisedDevice *>(dev));
  }
};
static ScanCallbacks g_scanCallbacks;

// ---- Client (connection) callback -----------------------------------------
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *) override { if (g_self) g_self->handleConnect(true); }
  void onDisconnect(NimBLEClient *, int) override { if (g_self) g_self->handleDisconnect(); }
};
static ClientCallbacks g_clientCallbacks;

// ---- Notification trampoline ----------------------------------------------
static void notifyCb(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  if (g_self) g_self->handleNotify(data, len);
}

void El15Client::begin() {
  g_self = this;
  NimBLEDevice::init("EL15-Controller");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
}

void El15Client::setState(State s, const char *info) {
  state_ = s;
  if (onState) onState(s, info);
}

// ---- Scanning --------------------------------------------------------------
void El15Client::startScan(uint32_t seconds) {
  setState(SCANNING, "Scanning…");
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

void El15Client::handleScanResult(NimBLEAdvertisedDevice *dev) {
  // The EL15 often omits FFF0 from its advert, so surface every *named*
  // device and verify the service after connecting (like the Android scan).
  std::string name = dev->getName();
  if (name.empty()) return;
  if (onDeviceFound) onDeviceFound(dev->getAddress().toString().c_str(), name.c_str());
}

// ---- Connection ------------------------------------------------------------
bool El15Client::connectTo(const char *address) {
  stopScan();
  setState(CONNECTING, "Connecting…");
  if (!client_) {
    client_ = NimBLEDevice::createClient();
    client_->setClientCallbacks(&g_clientCallbacks, false);
  }
  NimBLEAddress addr(address, BLE_ADDR_PUBLIC);
  if (!client_->connect(addr)) {
    setState(IDLE, "Connect failed");
    return false;
  }
  return true;  // service discovery continues in handleConnect
}

void El15Client::handleConnect(bool) {
  // Resolve the FFF0 service and its notify/write characteristics.
  NimBLERemoteService *svc = client_->getService(el15::SERVICE_UUID);
  if (!svc) { disconnect(); setState(IDLE, "Not an EL15 (no FFF0)"); return; }
  notifyChar_ = svc->getCharacteristic(el15::NOTIFY_UUID);
  writeChar_ = svc->getCharacteristic(el15::WRITE_UUID);
  if (!notifyChar_ || !writeChar_) { disconnect(); setState(IDLE, "EL15 characteristics missing"); return; }
  if (notifyChar_->canNotify()) notifyChar_->subscribe(true, notifyCb);
  frameLen_ = 0;
  lastPollMs_ = 0;
  setState(CONNECTED, "Connected · FFF0");
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
  if (state_ != CONNECTED) return;
  uint32_t now = millis();
  if (now - lastPollMs_ >= pollIntervalMs) {
    lastPollMs_ = now;
    poll();
  }
}
