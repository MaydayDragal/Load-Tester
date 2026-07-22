// Audio feedback for the EL15 controller — drives the on-board ES8311 codec +
// speaker amp over I2S. All the play* calls are non-blocking: they enqueue a
// short tone sequence onto a background task, so they are safe to call from the
// LVGL/loop context (UI taps, button events, fault/test callbacks).
//
// Loudness is controlled by scaling the synthesised PCM (setVolume / setMuted),
// so no codec I2C traffic happens at runtime — the shared I2C bus is only
// touched once, during begin().
#pragma once

#include <stdint.h>

namespace audio {

// Bring up the amp, I2S and ES8311. Non-fatal on failure (calls just no-op).
void begin();
bool ready();

// 0..100. Applied to the synthesised amplitude; 0 is silent.
void setVolume(uint8_t pct);
uint8_t getVolume();
void setMuted(bool m);
bool muted();

// ---- Semantic feedback (non-blocking) --------------------------------------
void click();     // soft tick — UI taps
void press();     // firmer confirm — physical button
void success();   // rising two-tone — test complete
void failure();   // falling two-tone — test error / abort
void fault();     // urgent alarm — protection trip / emergency stop

// Low-level: one tone. freqHz 0 = silence (a gap); ampPct 0..100 before volume.
void tone(uint16_t freqHz, uint16_t durMs, uint8_t ampPct);

}  // namespace audio
