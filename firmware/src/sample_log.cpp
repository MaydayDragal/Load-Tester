#include "sample_log.h"

#include <Arduino.h>
#include <LittleFS.h>

namespace samplelog {
namespace {

bool g_mounted = false;

}  // namespace

// Paths are versioned by test type so the two engines can never truncate each
// other's pending datapoints (a report streams from its log at SAVE time,
// which can be long after the run if the auto-save failed).
Log batt("/batt.log", 2000, 8000);
Log rtest("/rtest.log", 25, 8000);

bool begin() {
  if (g_mounted) return true;
  // formatOnFail: a blank or previously-SPIFFS partition formats itself once
  // rather than disabling logging forever.
  if (!LittleFS.begin(true)) {
    Serial.println("[log] LittleFS mount FAILED - per-sample logging disabled");
    return false;
  }
  g_mounted = true;
  Serial.printf("[log] LittleFS ready (%u kB total, %u kB used)\n",
                (unsigned)(LittleFS.totalBytes() / 1024),
                (unsigned)(LittleFS.usedBytes() / 1024));
  return true;
}

void end() {
  if (!g_mounted) return;
  batt.flush();
  rtest.flush();
  LittleFS.end();
  g_mounted = false;
}

bool Log::start() {
  if (!samplelog::begin()) return false;
  File f = LittleFS.open(path_, "w");   // truncate any previous run
  if (!f) {
    Serial.printf("[log] cannot open %s for writing\n", path_);
    return false;
  }
  f.close();
  batchN_ = 0;
  count_ = 0;
  intervalMs_ = baseMs_;
  nextAtMs_ = 0;
  tierLeft_ = maxRecords_ / 2;
  active_ = true;
  return true;
}

void Log::nextTier() {
  uint32_t remaining = maxRecords_ > count_ ? maxRecords_ - count_ : 0;
  tierLeft_ = remaining / 2;
  if (tierLeft_ == 0) tierLeft_ = remaining;   // final tier: spend what's left
  intervalMs_ *= 2;
  Serial.printf("[log] %s tier -> %lu ms interval, %lu records allowed\n",
                path_, (unsigned long)intervalMs_, (unsigned long)tierLeft_);
}

void Log::flush() {
  if (!g_mounted || batchN_ == 0) return;
  File f = LittleFS.open(path_, "a");
  if (!f) {
    Serial.printf("[log] append to %s failed - dropping this batch\n", path_);
    batchN_ = 0;
    return;
  }
  f.write((const uint8_t *)batch_, sizeof(Rec) * batchN_);
  f.close();
  batchN_ = 0;
}

void Log::add(uint32_t tMs, float v, float i, float temp, float aux0, float aux1) {
  if (!active_) return;
  // Tier schedule: store only when this sample is due. `>=` (not ==) because
  // the poll cadence never lands exactly on the interval boundary.
  if (count_ > 0 && tMs < nextAtMs_) return;
  if (count_ >= maxRecords_) return;   // budget exhausted (only after many tiers)
  if (tierLeft_ == 0) nextTier();
  if (tierLeft_ == 0) return;

  batch_[batchN_++] = Rec{tMs, v, i, temp, aux0, aux1};
  count_++;
  tierLeft_--;
  nextAtMs_ = tMs + intervalMs_;
  if (batchN_ >= BATCH) flush();
}

bool Log::replay(const std::function<bool(const Rec &)> &fn) {
  if (!g_mounted) return false;
  flush();   // the tail of the run lives in RAM until now
  File f = LittleFS.open(path_, "r");
  if (!f) return false;
  // Read a batch at a time: one 768 B stack buffer instead of a read() per
  // record, which matters when this streams thousands of them onto a slow card.
  Rec buf[BATCH];
  bool ok = true;
  while (f.available() >= (int)sizeof(Rec)) {
    size_t got = f.read((uint8_t *)buf, sizeof(buf));
    size_t n = got / sizeof(Rec);
    for (size_t k = 0; k < n; k++) {
      if (!fn(buf[k])) { ok = false; break; }
    }
    if (!ok) break;
    // Yield once per batch. The consumer is normally writing these rows to the
    // SD card over a slow bit-banged link, which can take tens of seconds in
    // total — long enough to starve the idle task and trip its watchdog. One
    // tick per 32 rows costs a few hundred ms overall and keeps the system fed.
    delay(1);
  }
  f.close();
  return ok;
}

}  // namespace samplelog
