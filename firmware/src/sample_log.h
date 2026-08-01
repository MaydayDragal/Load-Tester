// Per-sample datapoint log for the battery capacity test, buffered in the
// ESP32's own flash and copied to the SD card as CSV rows when the test ends.
//
// Why flash and not RAM: a capacity run is hours long. Even at 1 Hz a 12 h test
// is ~43 000 datapoints; this board has ~37 KB of free heap and no PSRAM, so the
// samples cannot live in RAM. Why not stream straight to the SD card: the card
// is on a ~250 kHz bit-banged link and every write blocks the loop task — doing
// that on every sample would stall the UI and the BLE poll for the whole run,
// and a card pulled mid-test would take the log with it. Internal flash is fast,
// always present, and survives a reboot.
//
// Bounded without ever discarding the run: the log is written in TIERS. Tier 0
// stores one sample per second until it has used half the record budget, then
// tier 1 stores one per two seconds until it has used half of what is left, and
// so on. Each tier therefore covers the same wall-clock span (~4 h), the file
// can never exceed the budget, and an arbitrarily long test still produces a
// complete curve — just with coarser resolution towards the end. Every record
// carries its own timestamp, so uneven spacing is self-describing in the CSV.
//
// Loop task only. Flash writes are buffered in RAM and only hit the flash once
// per BATCH records, so the per-sample cost is a struct copy.
#pragma once

#include <stdint.h>

#include <functional>

namespace samplelog {

// One datapoint. Power (V*I) and mAh (Ah*1000) are derived when the CSV is
// written rather than stored, so the on-flash record stays 24 bytes.
struct Rec {
  uint32_t tS;    // seconds since the discharge started (excludes paused time)
  float v;        // terminal voltage, V
  float i;        // current, A
  float temp;     // load temperature, degC
  float ah;       // integrated capacity so far, Ah
  float wh;       // integrated energy so far, Wh
};

// Mount the flash filesystem. Safe to call repeatedly; returns false if the
// partition is missing or cannot be formatted (logging then no-ops and the CSV
// simply carries no per-sample block — a test is never blocked by this).
bool begin();

// Release the filesystem and its RAM. The recorded log survives (it is a file).
void end();

// Begin a new run: mounts if needed, truncates any previous log, resets the
// tier schedule. Returns false if the log is unavailable.
bool start();

// Offer a datapoint. Cheap to call on every status packet — the tier schedule
// decides whether this one is actually stored, so the caller does not have to
// rate-limit. `tS` must be the run's active elapsed seconds (paused time
// excluded) so the timeline stays monotonic across a pause.
void add(uint32_t tS, float v, float i, float temp, float ah, float wh);

// Push the RAM batch to flash. Called automatically when the batch fills; call
// it explicitly at the end of a run (and before reading back) so the tail is
// not lost.
void flush();

// Stream every stored record in order. `fn` returns false to stop early.
// Returns false if the log could not be read.
bool replay(const std::function<bool(const Rec &)> &fn);

// Records stored so far, and the interval the current tier is recording at.
uint32_t count();
uint32_t intervalS();

}  // namespace samplelog
