#include "prefs.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

namespace prefs {
namespace {

Preferences g_nvs;
Data g_data;
bool g_dirty = false;
uint32_t g_dirtyAt = 0;

// How long values must sit unchanged before they are committed. Long enough
// that a slider drag or a keypad edit is one write, short enough that a power
// cut right after a change rarely loses it.
const uint32_t SETTLE_MS = 1500;

// Keys are short by NVS convention (15 chars max) and must never be renamed —
// a rename silently resets that setting for every existing device.
void writeAll() {
  g_nvs.putUChar("bright", g_data.brightness);
  g_nvs.putUChar("vol", g_data.volume);
  g_nvs.putBool("mute", g_data.muted);
  g_nvs.putUShort("idleDim", g_data.idleDimS);
  g_nvs.putBool("pxShift", g_data.pixelShift);
  g_nvs.putUShort("pollMs", g_data.pollMs);
  g_nvs.putFloat("fuse", g_data.fuseRating);
  g_nvs.putUChar("rtSteps", g_data.rtSteps);
  g_nvs.putBool("fourWire", g_data.fourWire);
  g_nvs.putFloat("tare", g_data.tareOhm);
  g_nvs.putUChar("btChem", g_data.battChem);
  g_nvs.putUChar("btCells", g_data.battCells);
  g_nvs.putFloat("btCut", g_data.battCutoff);
  g_nvs.putBool("btCutCust", g_data.battCutoffCustom);
  g_nvs.putFloat("btAmps", g_data.battAmps);
  g_nvs.putString("ssid", g_data.ssid);
  g_nvs.putString("pass", g_data.pass);
  g_nvs.putShort("tz", g_data.tzMinutes);
  g_nvs.putString("lastAddr", g_data.lastAddr);
  g_nvs.putUChar("lastAddrT", g_data.lastAddrType);
}

}  // namespace

void begin() {
  if (!g_nvs.begin("el15", false)) {
    Serial.println("[prefs] NVS open failed - running on defaults");
    return;
  }
  Data d;   // defaults, used for every key that is not stored yet
  g_data.brightness = g_nvs.getUChar("bright", d.brightness);
  g_data.volume = g_nvs.getUChar("vol", d.volume);
  g_data.muted = g_nvs.getBool("mute", d.muted);
  g_data.idleDimS = g_nvs.getUShort("idleDim", d.idleDimS);
  g_data.pixelShift = g_nvs.getBool("pxShift", d.pixelShift);
  g_data.pollMs = g_nvs.getUShort("pollMs", d.pollMs);
  g_data.fuseRating = g_nvs.getFloat("fuse", d.fuseRating);
  g_data.rtSteps = g_nvs.getUChar("rtSteps", d.rtSteps);
  g_data.fourWire = g_nvs.getBool("fourWire", d.fourWire);
  g_data.tareOhm = g_nvs.getFloat("tare", d.tareOhm);
  g_data.battChem = g_nvs.getUChar("btChem", d.battChem);
  g_data.battCells = g_nvs.getUChar("btCells", d.battCells);
  g_data.battCutoff = g_nvs.getFloat("btCut", d.battCutoff);
  g_data.battCutoffCustom = g_nvs.getBool("btCutCust", d.battCutoffCustom);
  g_data.battAmps = g_nvs.getFloat("btAmps", d.battAmps);
  g_nvs.getString("ssid", g_data.ssid, sizeof(g_data.ssid));
  g_nvs.getString("pass", g_data.pass, sizeof(g_data.pass));
  g_data.tzMinutes = g_nvs.getShort("tz", d.tzMinutes);
  g_nvs.getString("lastAddr", g_data.lastAddr, sizeof(g_data.lastAddr));
  g_data.lastAddrType = g_nvs.getUChar("lastAddrT", d.lastAddrType);
  g_data.inFlight = g_nvs.getUChar("inFlight", Data::NONE);
  Serial.printf("[prefs] loaded (inFlight=%u, ssid=%s)\n",
                (unsigned)g_data.inFlight, g_data.ssid[0] ? g_data.ssid : "-");
}

const Data &get() { return g_data; }
Data &mutable_() { return g_data; }

void markDirty() {
  g_dirty = true;
  g_dirtyAt = millis();
}

void tick() {
  if (!g_dirty || millis() - g_dirtyAt < SETTLE_MS) return;
  flush();
}

void flush() {
  if (!g_dirty) return;
  g_dirty = false;
  writeAll();
}

// The in-flight flag is deliberately NOT debounced: it exists to survive the
// crash that might happen in the next millisecond.
void armInFlight(uint8_t what) {
  if (g_data.inFlight == what) return;
  g_data.inFlight = what;
  g_nvs.putUChar("inFlight", what);
}

void clearInFlight() { armInFlight(Data::NONE); }

}  // namespace prefs
