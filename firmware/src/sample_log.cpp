#include "sample_log.h"

#include <Arduino.h>
#include <LittleFS.h>

namespace samplelog {
namespace {

const char *PATH = "/batt.log";

// Record budget. The binding constraint is NOT the 896 KB `spiffs` partition
// (8 000 x 24 B is only 192 KB) — it is how long the CSV takes to reach the card
// afterwards. The SD link is bit-banged at ~250 kHz, so every 1 000 rows of CSV
// costs roughly two seconds of blocked loop task; 8 000 rows is about as much as
// can be written without the save feeling broken. It is also about as many
// points as a spreadsheet will plot usefully.
const uint32_t MAX_RECORDS = 8000;
// 2 s: a discharge curve is a slow-moving thing, and this keeps a typical 2 h
// test entirely inside tier 0 (i.e. uniformly sampled) while still covering a
// 12 h run without exhausting the budget.
const uint32_t BASE_INTERVAL_S = 2;

// RAM staging: one flash write per BATCH records instead of one per sample.
// 32 x 24 B = 768 B, which is affordable on a board with ~37 KB free heap.
const int BATCH = 32;

bool g_mounted = false;
bool g_active = false;
Rec g_batch[BATCH];
int g_batchN = 0;
uint32_t g_count = 0;
uint32_t g_interval = BASE_INTERVAL_S;
uint32_t g_nextAtS = 0;
// Records still allowed in the current tier. When it runs out the interval
// doubles and the next tier gets half of whatever budget is left, so the total
// is bounded by MAX_RECORDS however long the test runs while each tier still
// covers the same wall-clock span.
uint32_t g_tierLeft = 0;

void nextTier() {
  uint32_t remaining = MAX_RECORDS > g_count ? MAX_RECORDS - g_count : 0;
  g_tierLeft = remaining / 2;
  if (g_tierLeft == 0) g_tierLeft = remaining;   // final tier: spend what's left
  g_interval *= 2;
  Serial.printf("[log] tier -> %lu s interval, %lu records allowed\n",
                (unsigned long)g_interval, (unsigned long)g_tierLeft);
}

}  // namespace

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
  flush();
  LittleFS.end();
  g_mounted = false;
  g_active = false;
}

bool start() {
  if (!begin()) return false;
  File f = LittleFS.open(PATH, "w");   // truncate any previous run
  if (!f) {
    Serial.println("[log] cannot open the sample log for writing");
    return false;
  }
  f.close();
  g_batchN = 0;
  g_count = 0;
  g_interval = BASE_INTERVAL_S;
  g_nextAtS = 0;
  g_tierLeft = MAX_RECORDS / 2;
  g_active = true;
  return true;
}

void flush() {
  if (!g_mounted || g_batchN == 0) return;
  File f = LittleFS.open(PATH, "a");
  if (!f) {
    Serial.println("[log] append failed - dropping this batch");
    g_batchN = 0;
    return;
  }
  f.write((const uint8_t *)g_batch, sizeof(Rec) * g_batchN);
  f.close();
  g_batchN = 0;
}

void add(uint32_t tS, float v, float i, float temp, float ah, float wh) {
  if (!g_active) return;
  // Tier schedule: store only when this sample is due. `>=` (not ==) because
  // the poll cadence never lands exactly on a second boundary.
  if (g_count > 0 && tS < g_nextAtS) return;
  if (g_count >= MAX_RECORDS) return;   // budget exhausted (only after many tiers)
  if (g_tierLeft == 0) nextTier();
  if (g_tierLeft == 0) return;

  g_batch[g_batchN++] = Rec{tS, v, i, temp, ah, wh};
  g_count++;
  g_tierLeft--;
  g_nextAtS = tS + g_interval;
  if (g_batchN >= BATCH) flush();
}

bool replay(const std::function<bool(const Rec &)> &fn) {
  if (!g_mounted) return false;
  flush();   // the tail of the run lives in RAM until now
  File f = LittleFS.open(PATH, "r");
  if (!f) return false;
  // Read a batch at a time: one 768 B stack buffer instead of a read() per
  // record, which matters when this streams 30 000 of them onto a slow card.
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

uint32_t count() { return g_count; }
uint32_t intervalS() { return g_interval; }

}  // namespace samplelog
