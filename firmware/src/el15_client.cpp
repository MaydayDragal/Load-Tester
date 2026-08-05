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
  // The reason code is the single most useful thing the stack tells us about a
  // dropped link — supervision timeout (an RF/timing problem) and "remote user
  // terminated" (the EL15 hung up on us deliberately) have completely different
  // fixes, and without this byte they are indistinguishable from the log. It
  // used to be discarded here.
  void onDisconnect(NimBLEClient *, int reason) override {
    if (g_self) g_self->enqueueDisconnected(reason);
  }
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

// Human-readable HCI disconnect reasons. NimBLE reports some codes offset into
// its own error space (BLE_HS_HCI_ERR adds 0x200), so mask that off first.
const char *El15Client::disconnectReason(int code) {
  switch (code & 0xFF) {
    case 0x08: return "supervision timeout (link went quiet)";
    case 0x13: return "remote terminated (the load hung up)";
    case 0x14: return "remote terminated - low resources";
    case 0x15: return "remote terminated - power off";
    case 0x16: return "local host terminated (we hung up)";
    case 0x22: return "LL response timeout";
    case 0x28: return "instant passed";
    case 0x3B: return "unacceptable connection parameters";
    case 0x3E: return "connection failed to be established";
    case 0x02: return "unknown connection identifier";
    case 0x05: return "authentication failure";
    case 0x06: return "PIN or key missing";
    case 0x00: return "(none reported)";
    default:   return "unrecognised";
  }
}

void El15Client::enqueueDisconnected(int reason) {
  if (!evtQueue_) return;
  Event e;
  e.kind = Event::DISCONNECTED;
  e.reason = (int16_t)reason;
  // Disconnect is rare and must not be lost, so allow a brief wait if the queue
  // is momentarily full — and if even that fails, latch a flag the loop task
  // checks, so the event is delayed at worst, never dropped.
  if (xQueueSend(evtQueue_, &e, pdMS_TO_TICKS(20)) != pdTRUE) {
    discPendingReason_ = (int16_t)reason;
    discPending_ = true;
  }
}

void El15Client::drainEvents() {
  if (!evtQueue_) return;
  Event e;
  while (xQueueReceive(evtQueue_, &e, 0) == pdTRUE) {
    switch (e.kind) {
      case Event::NOTIFY:       handleNotify(e.data, e.len); break;
      case Event::DISCONNECTED: handleDisconnect(e.reason); break;
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
  // Fallback for a DISCONNECTED event that could not be queued (queue full for
  // longer than enqueueDisconnected() was willing to wait).
  if (discPending_) {
    discPending_ = false;
    handleDisconnect(discPendingReason_);
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
  // Connecting while a link is already up used to wedge: connect() fails on an
  // already-connected client, the failure path set state_ = IDLE, and the OLD
  // link stayed live — telemetry kept flowing while every write (including the
  // safety LOAD_OFF) was silently refused. Reconnecting to the same peer is a
  // no-op; a different peer gets a clean LOAD_OFF + teardown first, and the
  // teardown's DISCONNECTED event is consumed HERE so it cannot wipe the new
  // connection's state from the queue later.
  if (client_ && client_->isConnected()) {
    if (state_ == CONNECTED && addr.toString() == lastAddr_) return true;
    shutdownAndDisconnect();
    uint32_t t0 = millis();
    while (client_->isConnected() && millis() - t0 < 1000) delay(10);
    delay(50);      // let the host task enqueue its DISCONNECTED event...
    drainEvents();  // ...and consume it before the new attempt begins
  }
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
  // Heap is printed on every attempt because a reconnect that fails for lack of
  // a ~30 KB contiguous block reports HCI 0x3e — "connection failed to be
  // established" — which is indistinguishable from a peer that is out of range.
  // Having the number next to the failure is what separates "move closer" from
  // "the heap fragmented, reboot".
  Serial.printf("[ble] connecting to %s (addr type %d) | heap %u B free, largest %u B\n",
                addr.toString().c_str(), addr.getType(),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  if (!client_->connect(addr)) {
    // rc 13 = BLE_HS_ETIMEOUT / HCI 0x3e: the peer never completed the
    // handshake (out of range, rotated its address, or an Android RPA peripheral
    // not accepting). Keep the client for reuse — deleting it here races the
    // controller's late disconnect event ("client not found").
    int rc = client_->getLastError();
    Serial.printf("[ble] connect() FAILED rc=%d (%s) | heap %u B free, largest %u B\n",
                  rc, disconnectReason(rc),
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    if (ESP.getMaxAllocHeap() < 30000)
      Serial.println("[ble] NOTE: largest free block is under the ~30 KB NimBLE needs to "
                     "connect - this is a MEMORY failure, not a radio one. A reboot will clear it.");
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
  // Telemetry is the whole point of the link: a characteristic that cannot
  // notify, or a CCCD write that fails, must fail the connect loudly — carrying
  // on used to report "Connected" with a permanently blank Monitor.
  if (!notifyChar_->canNotify() || !notifyChar_->subscribe(true, notifyCb)) {
    disconnect();
    setState(IDLE, "Subscribe failed");
    return false;
  }

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
  // Re-anchor the control-write poll hold. Left at its initial 0, the signed
  // comparison in loopTick() reads (int32_t)millis() — negative once uptime
  // passes 2^31 ms (~24.9 days), which would silently stop all polling (and
  // with it all telemetry) until a control write re-anchored it.
  pollHoldUntilMs_ = millis();
  snprintf(lastAddr_, sizeof(lastAddr_), "%s", addr.toString().c_str());
  lastAddrType_ = addr.getType();
  setState(CONNECTED, "Connected - FFF0");
  return true;
}

void El15Client::handleDisconnect(int reason) {
  // Heap is logged alongside the reason because the two failure modes look
  // identical from the outside: a link that DROPS is an RF/protocol problem,
  // whereas a link that will not COME BACK is usually a memory one — NimBLE
  // needs a ~30 KB contiguous block to establish a connection, and a fragmented
  // heap presents as "Connect failed" (HCI 0x3e), not as an out-of-memory error.
  Serial.printf("[ble] DISCONNECTED reason=0x%02X (%s) | heap %u B free, largest %u B\n",
                reason & 0xFF, disconnectReason(reason),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  notifyChar_ = nullptr;
  writeChar_ = nullptr;
  frameLen_ = 0;
  wantMode_ = -1;   // nothing outstanding survives the link
  modeTries_ = 0;
  lastMode_ = -1;
  setState(IDLE, "Disconnected");
}

void El15Client::disconnect() {
  if (client_ && client_->isConnected()) client_->disconnect();
  else handleDisconnect(-1);
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
    // (CTRL_POLL_GAP_MS, used below, is the smaller separation a POLL needs from
    // a control write — a poll is a different opcode, so it does not collide the
    // way two control writes do.)
    uint32_t since = millis() - lastCtrlMs;
    if (lastCtrlMs && since < CTRL_GAP_MS) delay(CTRL_GAP_MS - since);
    lastCtrlMs = millis();
  }

  bool withResp = writeChar_->canWrite();
  bool ok = writeChar_->writeValue(const_cast<uint8_t *>(d), n, withResp);
  // Keep the next poll clear of this command, but only by a short gap — NOT by a
  // whole poll interval, which is what resetting lastPollMs_ used to do. The
  // device drops a no-response write that lands right behind another one; it
  // does not need the line quiet for 50-500 ms.
  if (isCtrl) pollHoldUntilMs_ = millis() + ctrlPollGapMs;

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
    if (s.valid) checkModeConfirm(s);
    if (onStatus) onStatus(s);
    memmove(frameBuf_, frameBuf_ + 28, frameLen_ - 28);
    frameLen_ -= 28;
  }
}

// ---- Mode commands, confirmed ----------------------------------------------
// Measured on a real EL15 (2026-08-05, back-to-back mode sweep): 2 of 9 mode
// commands were silently discarded by the device even though every write
// returned OK. They have to be, because FFF3 is write-WITHOUT-response — the
// "OK" is the local stack accepting the bytes, and the load never acknowledges
// anything. Nothing detected the loss, so the UI showed one mode while the
// device sat in another, which reads exactly like a flaky connection.
//
// It matters well beyond cosmetics: CapacityTest::start() commands CC before
// discharging, and a dropped CC would have run an entire battery test in
// whatever mode the load happened to be left in.
//
// So the mode is now driven closed-loop against the telemetry that was already
// arriving: command it, watch the status packets, re-send if it did not take,
// and say so plainly if the device refuses it outright.
void El15Client::sendModeNow(int mode) {
  write(el15::modeCommand(mode));
  // Allow two poll periods plus a margin for the change to show up in telemetry:
  // at a 500 ms poll that is ~1.2 s, at a 20 Hz poll ~0.3 s. A fixed number
  // would either thrash at slow poll rates or crawl at fast ones.
  modeDeadlineMs_ = millis() + 2 * pollIntervalMs + 200;
}

void El15Client::setMode(int mode) {
  wantMode_ = (int16_t)mode;
  modeTries_ = 1;
  sendModeNow(mode);
}

void El15Client::checkModeConfirm(const el15::Status &s) {
  lastMode_ = (int16_t)s.mode;
  if (wantMode_ < 0) return;
  if (s.mode == wantMode_) {
    if (modeTries_ > 1)
      Serial.printf("[ble] mode 0x%02X confirmed after %u tries\n",
                    (unsigned)wantMode_, (unsigned)modeTries_);
    wantMode_ = -1;
    modeTries_ = 0;
  }
}

void El15Client::modeRetryTick() {
  if (wantMode_ < 0) return;
  if ((int32_t)(millis() - modeDeadlineMs_) < 0) return;
  if (modeTries_ >= MODE_MAX_TRIES) {
    // Out of retries. This is the honest end of the road: the device is not
    // taking this mode, and pretending otherwise would leave every downstream
    // consumer acting on a mode the load is not in.
    Serial.printf("[ble] mode 0x%02X REFUSED after %u tries - the load is still in 0x%02X\n",
                  (unsigned)wantMode_, (unsigned)modeTries_, (unsigned)(lastMode_ & 0xFF));
    wantMode_ = -1;
    modeTries_ = 0;
    return;
  }
  modeTries_++;
  Serial.printf("[ble] mode 0x%02X not taken (device in 0x%02X) - resend %u/%u\n",
                (unsigned)wantMode_, (unsigned)(lastMode_ & 0xFF),
                (unsigned)modeTries_, (unsigned)MODE_MAX_TRIES);
  sendModeNow(wantMode_);
}

void El15Client::poll() { writeFixed(el15::POLL, sizeof(el15::POLL)); }

void El15Client::loopTick() {
  drainEvents();  // process queued BLE events on THIS (loop) task
  // Reconcile state_ with the real link. A lost or stale DISCONNECTED event
  // can leave the two disagreeing in either direction, and both are unsafe: a
  // "connected" UI over a dead link never fires the link-guard chain, and a
  // live unmanaged link refuses the safety LOAD_OFF. Trust the controller, not
  // the bookkeeping.
  if (state_ == CONNECTED && client_ && !client_->isConnected()) handleDisconnect(-1);
  else if (state_ == IDLE && client_ && client_->isConnected()) client_->disconnect();
  if (state_ != CONNECTED) return;

  modeRetryTick();   // re-send a mode command the device did not take

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
    // Go through the PUBLIC entry points, not writeRaw directly: setMode() is
    // where the confirm-and-retry lives, and a self-test that bypassed it was
    // measuring a code path no caller actually uses.
    setSetpoint(0.0f);
    setMode(SWEEP[want].id);
  }
#endif

#ifndef EL15_NO_POLL
  uint32_t now = millis();
  if (now - lastPollMs_ >= pollIntervalMs &&
      (int32_t)(now - pollHoldUntilMs_) >= 0) {
    lastPollMs_ = now;
    poll();
  }
#endif
}
