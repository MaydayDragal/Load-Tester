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
// Returns true only when the file is synced and closed on the card. `msg` gets
// the bare file name on success, or a short user-facing failure reason ("No
// card detected", "Card not formatted (use FAT32)", ...) — the UI shows it
// verbatim, so a failed save is never reported as a save.
bool saveCsv(const char *prefix, const std::function<bool(Print &)> &body,
             char *msg, size_t msgLen);

// One-shot card probe for the Settings screen: mounts, reads the type/size and
// free space, unmounts. `msg` gets "SDHC 29.7 GB (28.9 GB free)" or the same
// failure reasons as above.
bool info(char *msg, size_t msgLen);

#ifdef EL15_SDTEST
// Test-only: read a file back and echo its first lines to serial, so a self-test
// can prove written bytes actually landed. Not part of the normal API.
bool readBackTest(const char *name, char *msg, size_t msgLen);
#endif

}  // namespace sd
