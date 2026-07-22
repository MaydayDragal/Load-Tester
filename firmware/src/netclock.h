// Wi-Fi NTP time sync for the PCF85063 RTC.
//
// The board has a Wi-Fi 6 radio that this firmware otherwise never turns on,
// and an RTC that nothing can set — so every SD report is stamped "(RTC not
// set) uptime NNN s". One sync fixes that permanently (the RTC keeps running on
// the main battery, and on the backup cell if one is ever fitted).
//
// The radio is only powered for the duration of a sync and switched off again:
// Wi-Fi and BLE share one antenna path on the C6, and the BLE link is the only
// way this controller can stop a load. For the same reason a sync is refused
// while a test is running — see the guard in ui.cpp.
//
// Non-blocking: start() returns immediately and tick() advances a state machine
// from the loop task, so the UI keeps drawing through the ~5-15 s it takes.
#pragma once

#include <stddef.h>

#include <functional>

namespace net {

// SCANNING is appended so DONE/FAILED keep their values (the UI maps 3=done,
// 4=failed); scan progress is reported through onScanDone, not onProgress.
enum State { IDLE, CONNECTING, SYNCING, DONE, FAILED, SCANNING };

// Progress for the UI. `text` is user-facing ("Connecting to <ssid>...",
// "Clock set to 2026-07-22 14:03", "Wi-Fi not found").
extern std::function<void(State state, const char *text)> onProgress;

// Begin a sync. tzMinutes is the offset from UTC to store in the RTC (the RTC
// holds local time; there is no zone information on the part).
// Returns false if a sync is already running or no SSID is configured.
bool start(const char *ssid, const char *pass, int tzMinutes);

// ---- Network scan ----------------------------------------------------------
// Async scan for nearby networks so the SSID can be picked instead of typed.
// Same radio discipline as start(): refused while busy, radio powered only for
// the scan and switched off again. Results (deduped, strongest-first, hidden
// SSIDs dropped) arrive via onScanDone; read them with scanCount()/scanSsid()/
// scanRssi() from inside that callback. Returns false if the radio is busy.
extern std::function<void(int count, const char *err)> onScanDone;
bool startScan();
int scanCount();
const char *scanSsid(int i);
int scanRssi(int i);

// Advance the state machine. Call from loop().
void tick();

State state();
bool busy();

}  // namespace net
