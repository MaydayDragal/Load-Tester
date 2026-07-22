// microSD (TF) card storage for test reports — see CAPACITY_PLAN.md Phase 0.
//
// The card shares the ESP32-C6's single SPI host with the AMOLED, so it is
// never left mounted: every access mounts, writes, and unmounts as one
// operation. That is why the API is a single saveCsv() call taking the body
// writer rather than an open/write/close file API — a caller cannot forget to
// unmount, and nothing can draw to the panel while the bus points at the card.
//
// All calls block for a few hundred ms (card init dominates) and must be made
// from the loop task.
#pragma once

#include <stddef.h>
#include <stdio.h>

#include <functional>

namespace sd {

// Write the next free numbered CSV for `prefix` — "RTEST" -> /RTEST_007.CSV,
// the index being one past the highest already on the card. body() writes the
// file contents and returns false if it could not.
//
// Returns true only when the file is closed and flushed on the card. `msg` gets
// the bare file name on success, or a short user-facing failure reason ("No
// card detected", "Card is full", ...) — the UI shows it verbatim, so a failed
// save is never reported as a save.
bool saveCsv(const char *prefix, const std::function<bool(FILE *)> &body,
             char *msg, size_t msgLen);

// One-shot card probe for the Settings screen: mounts, reads the name/size,
// unmounts. `msg` gets "SDHC 29.7 GB" or the same failure reasons as above.
bool info(char *msg, size_t msgLen);

}  // namespace sd
