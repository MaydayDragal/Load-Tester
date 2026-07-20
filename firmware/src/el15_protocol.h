// EL15 electronic-load BLE protocol — C++ port of El15Protocol.kt.
//
// Faithful reimplementation of the Android app's protocol layer so the ESP32
// speaks to the load byte-for-byte identically. Ported from the DM40GUI
// reference (github.com/maj113/DM40GUI, el15/protocol_constants.py).
//
// The device exposes a Nordic-style FFF0 service. Status packets are pushed as
// notifications on FFF1; command frames are written to FFF3.
#pragma once

#include <Arduino.h>
#include <string.h>
#include <vector>

namespace el15 {

// ---- GATT UUIDs (16-bit, expanded to the Bluetooth base UUID) -------------
static const char *SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb";
static const char *NOTIFY_UUID  = "0000fff1-0000-1000-8000-00805f9b34fb";
static const char *WRITE_UUID   = "0000fff3-0000-1000-8000-00805f9b34fb";

// ---- Fixed command frames (captured, replayed verbatim) -------------------
static const uint8_t POLL[]     = {0xAF, 0x07, 0x03, 0x08, 0x00, 0x3F};
static const uint8_t LOAD_ON[]  = {0xAF, 0x07, 0x03, 0x09, 0x01, 0x04};
static const uint8_t LOAD_OFF[] = {0xAF, 0x07, 0x03, 0x09, 0x01, 0x00};
static const uint8_t LOCK[]     = {0xAF, 0x07, 0x03, 0x09, 0x01, 0x01};

static const uint8_t MODE_PREFIX[]     = {0xAF, 0x07, 0x03, 0x03, 0x01};
static const uint8_t SETPOINT_PREFIX[] = {0xAF, 0x07, 0x03, 0x04, 0x04};
static const uint8_t HEADER[]          = {0xDF, 0x07, 0x03, 0x08};

// ---- Modes ----------------------------------------------------------------
enum Mode {
  MODE_CC        = 0x01,
  MODE_CAP       = 0x02,
  MODE_DT        = 0x03,
  MODE_ADV       = 0x04,
  MODE_CV        = 0x09,
  MODE_DCR       = 0x0A,
  MODE_POWER     = 0x0B,
  MODE_ADV_SCAN  = 0x0C,
  MODE_POWER_RPT = 0x0D,
  MODE_CR        = 0x11,
  MODE_CP        = 0x19,
};

// ---- Hardware ratings (ALIENTEK EL15: 150 W / 60 V / 12 A) -----------------
static const float MAX_CURRENT_A = 12.0f;
static const float MAX_POWER_W   = 150.0f;
static const float MAX_VOLTAGE_V = 60.0f;
static const float MIN_VOLTAGE_V = 0.1f;
static const int   FAN_SPEED_MAX = 5;

inline const char *modeName(int mode) {
  switch (mode) {
    case MODE_CC: return "CC";
    case MODE_CAP: return "CAP";
    case MODE_DT: return "POW [DT]";
    case MODE_ADV: return "ADV [L]";
    case MODE_CV: return "CV";
    case MODE_DCR: return "DCR";
    case MODE_POWER: return "POW [A]";
    case MODE_ADV_SCAN: return "ADV [S]";
    case MODE_POWER_RPT: return "POW [RPT]";
    case MODE_CR: return "CR";
    case MODE_CP: return "CP";
    default: return "?";
  }
}

struct SetpointInfo { const char *unit; int decimals; const char *label; };

inline SetpointInfo setpointInfo(int mode) {
  switch (mode) {
    case MODE_CC:  return {"A", 3, "Current"};
    case MODE_CAP: return {"A", 3, "Current"};
    case MODE_CV:  return {"V", 3, "Voltage"};
    case MODE_DCR: return {"A", 3, "Current"};
    case MODE_CR:  return {"\xCE\xA9", 1, "Resistance"};  // UTF-8 Ω
    case MODE_CP:  return {"W", 2, "Power"};
    default:       return {"?", 3, "Setpoint"};
  }
}

// Modes the user can pick from the UI (those with a meaningful setpoint).
static const int SELECTABLE_MODES[] = {MODE_CC, MODE_CV, MODE_CR, MODE_CP, MODE_CAP, MODE_DCR};
static const int SELECTABLE_MODES_N = 6;

// ---- Command builders -----------------------------------------------------
using Frame = std::vector<uint8_t>;

inline Frame modeCommand(int mode) {
  Frame f(MODE_PREFIX, MODE_PREFIX + sizeof(MODE_PREFIX));
  f.push_back((uint8_t)mode);
  return f;
}

// value encoded as little-endian IEEE-754 float32.
inline Frame setpointCommand(float value) {
  Frame f(SETPOINT_PREFIX, SETPOINT_PREFIX + sizeof(SETPOINT_PREFIX));
  uint8_t b[4];
  memcpy(b, &value, 4);  // ESP32 is little-endian, matching the wire format
  f.insert(f.end(), b, b + 4);
  return f;
}

// ---- Decoded status snapshot ----------------------------------------------
struct Status {
  bool  crcPass = false;
  bool  valid   = false;

  float voltage = 0, current = 0, power = 0;
  int   runtime = 0;
  float temperature = 0, setpoint = 0;
  float energyWh = 0, capacityAh = 0;
  float dcrMilliOhm = 0, dcrI1 = 0, dcrI2 = 0;

  int   mode = MODE_CC;
  const char *modeStr = "---";
  int   fanSpeed = 0;
  bool  loadOn = false, lockOn = false, ready = false;

  const char *setpointUnit = "A";
  int   setpointDecimals = 3;
  const char *setpointLabel = "Current";
  bool  setpointInPacket = true;
  char  warning[12] = "";
};

inline float f32(const uint8_t *d, int off) { float v; memcpy(&v, d + off, 4); return v; }
inline int32_t i32(const uint8_t *d, int off) { int32_t v; memcpy(&v, d + off, 4); return v; }

inline bool isStatusPacket(const uint8_t *d, int len) {
  return len >= 4 && memcmp(d, HEADER, 4) == 0;
}

// Parse a 28-byte EL15 status notification. Mirrors El15Protocol.parseStatus.
inline Status parseStatus(const uint8_t *d, int len) {
  Status s;
  int sum = 0;
  for (int i = 0; i < len; i++) sum += d[i];
  s.crcPass = (sum & 0xFF) == 0;
  if (len < 28 || !isStatusPacket(d, len) || !s.crcPass) return s;

  s.voltage = f32(d, 7);
  s.current = f32(d, 11);
  int runtime = i32(d, 15);
  s.power = s.voltage * s.current;

  int b5 = d[5] & 0xFF;
  int b6 = d[6] & 0xFF;

  const int B5_WARN_FLAG = 0x06, MODE_MASK = 0x1F;
  bool warnFlag = (b5 & B5_WARN_FLAG) == B5_WARN_FLAG;
  int rawMode = warnFlag ? (b5 & (MODE_MASK & ~B5_WARN_FLAG)) : (b5 & MODE_MASK);
  int mode = (strcmp(modeName(rawMode), "?") != 0) ? rawMode : (rawMode | 0x01);
  s.mode = mode;

  if (warnFlag) {
    int warnCode = b6 >> 4;
    if (warnCode == 0x6) strcpy(s.warning, "REV");
    else if (warnCode == 0x9) strcpy(s.warning, "UVP");
    else snprintf(s.warning, sizeof(s.warning), "PROT %X", warnCode);
    s.ready = false;
  } else {
    s.ready = (rawMode & 0x01) != 0 ||
        mode == MODE_CAP || mode == MODE_DCR || mode == MODE_ADV ||
        mode == MODE_POWER || mode == MODE_DT || mode == MODE_ADV_SCAN ||
        mode == MODE_POWER_RPT;
  }

  switch (mode) {
    case MODE_CAP:
      s.energyWh = f32(d, 19) * 0.001f;
      s.capacityAh = f32(d, 23) * 0.001f;
      s.setpointInPacket = false;
      break;
    case MODE_DCR:
      s.dcrI1 = f32(d, 15);
      s.dcrI2 = f32(d, 19);
      s.dcrMilliOhm = f32(d, 23);
      runtime = 0; s.current = 0; s.power = 0;
      s.setpointInPacket = false;
      break;
    case MODE_ADV: case MODE_POWER: case MODE_DT: case MODE_POWER_RPT:
      runtime = 0;
      s.setpointInPacket = false;
      break;
    default:
      s.temperature = f32(d, 19);
      s.setpoint = f32(d, 23);
      break;
  }
  s.runtime = runtime;

  // Fan speed spans two bytes: byte5 bits 6-7 -> low bits, byte6 bit0 -> MSB.
  s.fanSpeed = (b5 >> 6) | ((b6 & 0x01) << 2);
  s.loadOn = (b6 & 0x02) != 0;
  s.lockOn = (b6 & 0x04) != 0;
  s.modeStr = modeName(mode);

  SetpointInfo info = setpointInfo(mode);
  s.setpointUnit = info.unit;
  s.setpointDecimals = info.decimals;
  s.setpointLabel = info.label;

  s.valid = true;
  return s;
}

}  // namespace el15
