// Per-sample datapoint logs for the test engines, buffered in the ESP32's own
// flash and copied to the SD card as CSV rows when a test ends.
//
// Why flash and not RAM: a capacity run is hours long. Even at 1 Hz a 12 h test
// is ~43 000 datapoints; this board has ~37 KB of free heap and no PSRAM, so the
// samples cannot live in RAM. Why not stream straight to the SD card: the card
// is on a ~250 kHz bit-banged link and every write blocks the loop task — doing
// that on every sample would stall the UI and the BLE poll for the whole run,
// and a card pulled mid-test would take the log with it. Internal flash is fast,
// always present, and survives a reboot.
//
// Bounded without ever discarding the run: each log is written in TIERS. Tier 0
// stores one sample per base interval until it has used half the record budget,
// then tier 1 stores one per two intervals until it has used half of what is
// left, and so on. Each tier therefore covers the same wall-clock span, the file
// can never exceed the budget, and an arbitrarily long run still produces a
// complete curve — just with coarser resolution towards the end. Every record
// carries its own timestamp, so uneven spacing is self-describing in the CSV.
//
// TWO independent logs, one per test engine, in separate flash files:
//   batt  — the capacity test. 2 s base interval: a discharge curve is a
//           slow-moving thing, and this keeps a typical 2 h test entirely in
//           tier 0 while a 12 h run still fits the budget.
//   rtest — the resistance sweep. 25 ms base interval — BELOW the 50 ms poll
//           period, so tier 0 stores literally every status packet; a sweep of
//           up to ~3 min is captured at full rate and only the rare longer
//           sweep coarsens. A separate file, not a shared one, because either
//           report streams from its log AT SAVE TIME: a shared log would let a
//           quick R-test wipe the datapoints of a capacity run whose save is
//           still pending (failed auto-save, card not inserted yet).
// The two payload floats mean different things per engine — see Rec.
//
// Loop task only. Flash writes are buffered in RAM and only hit the flash once
// per BATCH records, so the per-sample cost is a struct copy.
#pragma once

#include <stdint.h>

#include <functional>

namespace samplelog {

// One datapoint, 24 bytes on flash. Derivable values (power = V*I, mAh =
// Ah*1000) are computed when the CSV is written rather than stored.
struct Rec {
  uint32_t tMs;   // ms since the run started (capacity: active time, pauses excluded)
  float v;        // terminal voltage, V
  float i;        // current, A
  float temp;     // load temperature, degC
  float aux0;     // batt: integrated capacity so far, Ah | rtest: commanded target, A
  float aux1;     // batt: integrated energy so far, Wh   | rtest: samples in the level
  float aux2;     // batt: unused                          | rtest: current spread across them
};

// Mount the flash filesystem shared by both logs. Safe to call repeatedly;
// returns false if the partition is missing or cannot be formatted (logging
// then no-ops and the CSV simply carries no per-sample block — a test is never
// blocked by this).
bool begin();

// Release the filesystem and its RAM. The recorded logs survive (they are files).
void end();

// One tiered flash log. Instances below — this is not meant to be constructed
// elsewhere (each instance owns a fixed flash path).
class Log {
 public:
  Log(const char *path, uint32_t baseIntervalMs, uint32_t maxRecords)
      : path_(path), baseMs_(baseIntervalMs), maxRecords_(maxRecords) {}

  // Begin a new run: mounts if needed, truncates any previous log, resets the
  // tier schedule. Returns false if the log is unavailable.
  bool start();

  // Offer a datapoint. Cheap to call on every status packet — the tier schedule
  // decides whether this one is actually stored, so the caller does not have to
  // rate-limit. `tMs` must be the run's elapsed milliseconds (for the capacity
  // test: active time, so the timeline stays monotonic across a pause).
  void add(uint32_t tMs, float v, float i, float temp, float aux0, float aux1,
           float aux2 = 0);

  // Push the RAM batch to flash. Called automatically when the batch fills; call
  // it explicitly at the end of a run (and before reading back) so the tail is
  // not lost.
  void flush();

  // Stream every stored record in order. `fn` returns false to stop early.
  // Returns false if the log could not be read.
  bool replay(const std::function<bool(const Rec &)> &fn);

  // Records stored so far, and the interval the current tier is recording at.
  uint32_t count() const { return count_; }
  uint32_t intervalMs() const { return intervalMs_; }

 private:
  static const int BATCH = 32;   // one flash write per BATCH records

  void nextTier();

  const char *path_;
  uint32_t baseMs_;
  uint32_t maxRecords_;

  bool active_ = false;
  Rec batch_[BATCH];
  int batchN_ = 0;
  uint32_t count_ = 0;
  uint32_t intervalMs_ = 0;
  uint32_t nextAtMs_ = 0;
  // Records still allowed in the current tier. When it runs out the interval
  // doubles and the next tier gets half of whatever budget is left, so the
  // total is bounded by maxRecords_ however long the run goes while each tier
  // still covers the same wall-clock span.
  uint32_t tierLeft_ = 0;
};

// The instances. Budgets are bound by the SD save, not the flash partition
// (8 000 x 24 B is 192 KB per log against an 896 KB partition): the CSV goes
// out over a ~250 kHz bit-banged link at roughly two seconds per 1 000 rows,
// and ~16 s of blocked loop is about as much as a save can take without
// feeling broken. It is also about as many points as a spreadsheet plots.
extern Log batt;    // capacity test:    2 s base interval
extern Log rtest;   // resistance sweep: 25 ms base interval (every packet)

}  // namespace samplelog
