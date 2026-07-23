#include "netclock.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "display.h"

namespace net {

std::function<void(State, const char *)> onProgress;
std::function<void(int, const char *)> onScanDone;

namespace {

// Generous but bounded: a wrong password fails as a timeout on ESP32, so these
// are what stands between the user and a hang.
const uint32_t CONNECT_TIMEOUT_MS = 15000;
const uint32_t SNTP_TIMEOUT_MS = 12000;
const uint32_t SCAN_TIMEOUT_MS = 12000;
// Any plausible "the clock is real now" threshold: 2024-01-01.
const time_t SANE_EPOCH = 1704067200;

State g_state = IDLE;
uint32_t g_startedMs = 0;
int g_tzMinutes = 0;
char g_ssid[33] = "";

// Scan is two-phase: bring the STA interface up, THEN start the scan once the
// driver reports started — esp_wifi_scan_start fails if called before the
// STA-start event lands, which is the usual "scan failed" on a cold radio with
// BLE already running.
bool g_scanKicked = false;
int g_scanRetries = 0;
uint32_t g_scanNextTryMs = 0;
const int SCAN_START_RETRIES = 6;
const uint32_t STA_START_TIMEOUT_MS = 4000;

// Scan results, deduped and sorted strongest-first.
const int MAX_SCAN = 24;
char g_scanSsid[MAX_SCAN][33];
int g_scanRssi[MAX_SCAN];
int g_scanN = 0;

void report(State s, const char *text) {
  g_state = s;
  if (onProgress) onProgress(s, text);
}

// Always leave the radio off: it shares the antenna path with BLE, which is the
// only channel that can command the load.
void radioOff() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

void finish(State s, const char *text) {
  radioOff();
  report(s, text);
}

void finishScan(const char *err) {
  radioOff();
  g_state = IDLE;
  if (onScanDone) onScanDone(err ? 0 : g_scanN, err);
}

// Fold the raw scan into g_scanSsid/g_scanRssi: drop hidden (empty) SSIDs, keep
// the strongest sighting of each duplicate, then insertion-sort by RSSI.
void collectScan(int n) {
  g_scanN = 0;
  for (int i = 0; i < n && g_scanN < MAX_SCAN; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    int r = WiFi.RSSI(i);
    int at = -1;
    for (int j = 0; j < g_scanN; j++)
      if (strcmp(g_scanSsid[j], s.c_str()) == 0) { at = j; break; }
    if (at >= 0) { if (r > g_scanRssi[at]) g_scanRssi[at] = r; continue; }
    snprintf(g_scanSsid[g_scanN], sizeof(g_scanSsid[0]), "%s", s.c_str());
    g_scanRssi[g_scanN] = r;
    g_scanN++;
  }
  for (int a = 1; a < g_scanN; a++) {
    char sk[33]; snprintf(sk, sizeof(sk), "%s", g_scanSsid[a]);
    int rk = g_scanRssi[a];
    int b = a - 1;
    while (b >= 0 && g_scanRssi[b] < rk) {
      snprintf(g_scanSsid[b + 1], sizeof(g_scanSsid[0]), "%s", g_scanSsid[b]);
      g_scanRssi[b + 1] = g_scanRssi[b];
      b--;
    }
    snprintf(g_scanSsid[b + 1], sizeof(g_scanSsid[0]), "%s", sk);
    g_scanRssi[b + 1] = rk;
  }
}

}  // namespace

bool start(const char *ssid, const char *pass, int tzMinutes) {
  if (busy()) return false;
  if (!ssid || !ssid[0]) {
    report(FAILED, "No Wi-Fi network set");
    return false;
  }
  snprintf(g_ssid, sizeof(g_ssid), "%s", ssid);
  g_tzMinutes = tzMinutes;
  g_startedMs = millis();

  WiFi.persistent(false);   // credentials live in our own NVS, not the SDK's
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  char b[64];
  snprintf(b, sizeof(b), "Connecting to %s...", ssid);
  report(CONNECTING, b);
  return true;
}

bool startScan() {
  if (busy()) return false;
  g_scanN = 0;
  g_scanKicked = false;
  g_scanRetries = 0;
  g_scanNextTryMs = 0;   // first kick attempt as soon as the STA is started
  g_startedMs = millis();
  WiFi.persistent(false);
  bool modeOk = WiFi.mode(WIFI_STA);   // synchronous: runs esp_wifi_start
  if (!modeOk) Serial.println("[ntp] WiFi.mode(STA) failed");
  g_state = SCANNING;
  report(SCANNING, "Scanning for networks...");
  return true;
}

int scanCount() { return g_scanN; }
const char *scanSsid(int i) { return (i >= 0 && i < g_scanN) ? g_scanSsid[i] : ""; }
int scanRssi(int i) { return (i >= 0 && i < g_scanN) ? g_scanRssi[i] : -127; }

void tick() {
  if (g_state == SCANNING) {
    // Phase 1: wait for the STA interface to come up, then start the scan.
    if (!g_scanKicked) {
      if (!WiFi.STA.started()) {
        if (millis() - g_startedMs > STA_START_TIMEOUT_MS)
          finishScan("Wi-Fi did not start (radio busy?)");
        return;
      }
      if ((int32_t)(millis() - g_scanNextTryMs) < 0) return;  // spacing retries
      int16_t r = WiFi.scanNetworks(true /* async */, false /* no hidden */);
      if (r == WIFI_SCAN_RUNNING) {
        g_scanKicked = true;
        g_startedMs = millis();   // restart the timeout for the scan itself
        return;
      }
      if (++g_scanRetries >= SCAN_START_RETRIES) {
        Serial.printf("[ntp] scanNetworks would not start (last=%d)\n", (int)r);
        finishScan("Could not start scan - try again");
        return;
      }
      g_scanNextTryMs = millis() + 200;
      return;
    }
    // Phase 2: poll for completion.
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      if (millis() - g_startedMs > SCAN_TIMEOUT_MS) {
        WiFi.scanDelete();
        finishScan("Scan timed out");
      }
      return;
    }
    if (n < 0) { finishScan("Scan failed - try again"); return; }  // WIFI_SCAN_FAILED
    collectScan(n);
    WiFi.scanDelete();
    Serial.printf("[ntp] scan found %d network(s)\n", g_scanN);
    finishScan(nullptr);
    return;
  }
  if (g_state != CONNECTING && g_state != SYNCING) return;
  uint32_t elapsed = millis() - g_startedMs;

  if (g_state == CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      // UTC from the servers; the offset is applied when the RTC is written, so
      // the RTC holds local time (the part has no notion of a zone).
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      g_startedMs = millis();
      report(SYNCING, "Connected - asking for the time...");
      return;
    }
    if (elapsed > CONNECT_TIMEOUT_MS) {
      // Distinguish the two failures the user can actually act on.
      finish(FAILED, WiFi.status() == WL_NO_SSID_AVAIL
                         ? "Network not found - check the name"
                         : "Could not join - check the password");
    }
    return;
  }

  // SYNCING
  time_t now = time(nullptr);
  if (now > SANE_EPOCH) {
    time_t local = now + (time_t)g_tzMinutes * 60;
    struct tm tmv;
    gmtime_r(&local, &tmv);
    bool ok = display::setRtcTime(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    char b[64];
    if (ok)
      snprintf(b, sizeof(b), "Clock set: %04d-%02d-%02d %02d:%02d",
               tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    else
      snprintf(b, sizeof(b), "Got the time but the RTC did not accept it");
    Serial.printf("[ntp] %s\n", b);
    finish(ok ? DONE : FAILED, b);
    return;
  }
  if (elapsed > SNTP_TIMEOUT_MS)
    finish(FAILED, "No answer from the time servers");
}

State state() { return g_state; }
bool busy() { return g_state == CONNECTING || g_state == SYNCING || g_state == SCANNING; }

}  // namespace net
