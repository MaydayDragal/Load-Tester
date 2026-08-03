// microSD (TF) card storage for test reports — see CAPACITY_PLAN.md Phase 0.
//
// The card runs on bit-banged (software) SPI on its own dedicated pins, entirely
// independent of the AMOLED's SPI2 peripheral — the C6 has one hardware SPI host
// and the panel owns it, and the IDF sdspi driver cannot share it. See the long
// note in sd_card.cpp. The API is a single saveCsv() call taking a body writer
// rather than an open/write/close file API, so a caller cannot forget to unmount.
//
// All calls block the loop task for a few hundred ms (software SPI + card init)
// and must be made from the loop task.
#pragma once

#include <Arduino.h>   // Print
#include <stddef.h>

#include <functional>

namespace sd {

// Write the next free numbered CSV for `prefix` — "RTEST" -> /RTEST_007.CSV,
// the index being one past the highest already on the card. body() writes the
// file contents (via the Print interface — print()/println(), or the report.h
// fpf() printf helper) and returns false if it could not.
//
// Returns true only when the file has been synced, closed, AND read back off the
// card and checksummed against what we wrote. `msg` gets the bare file name on
// success, or a short user-facing failure reason ("No card detected", "Card not
// formatted (use FAT32)", "Card lost data twice - replace the card", ...) — the
// UI shows it verbatim, so a failed save is never reported as a save.
//
// The read-back is not optional and roughly DOUBLES the time a save takes (a
// 300 KB capacity report: ~11 s of blocked loop task becomes ~20 s). It is there
// because on 2026-08-03 a card acknowledged ~22 KB of writes it did not retain
// and raised no error anywhere — a checksummed read-back is the only thing that
// can catch a card which lies about what it kept. See sd_card.cpp.
bool saveCsv(const char *prefix, const std::function<bool(Print &)> &body,
             char *msg, size_t msgLen);

// One-shot card probe for the Settings screen. `msg` gets "SDHC 29.7 GB" or the
// same failure reasons as above. Always re-probes the physical card first, so
// this is also how a user confirms a freshly-inserted card was picked up.
bool info(char *msg, size_t msgLen);

// Forget the current mount so the next operation runs a full card init.
//
// The card is normally mounted once and left mounted (re-running init over the
// bit-banged link is flaky). That breaks the moment somebody ejects the card:
// SdFat keeps serving a cached FAT for a card that is no longer there, and a
// card that is pulled and re-inserted comes back in its native SD mode and
// ignores SPI commands until CMD0/ACMD41 run again. So every entry point probes
// the card (CMD10) before trusting the mount and calls this when it does not
// answer — a reinserted card then just works on the next save.
void invalidate();

#ifdef EL15_SDTEST
// Test-only: read a file back and echo its first lines to serial, so a self-test
// can prove written bytes actually landed. Not part of the normal API.
bool readBackTest(const char *name, char *msg, size_t msgLen);
#endif

}  // namespace sd
