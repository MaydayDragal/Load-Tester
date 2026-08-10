#include "display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <esp_heap_caps.h>

#include "board_config.h"

// Panel bring-up, per board. Both paths end at a single `g_gfx` that the rest of
// this file draws through; only brightness differs enough to need its own
// typed pointer (see setBrightness()).
#if BOARD_PANEL_QSPI_AMOLED
// SH8601 AMOLED over QSPI via Arduino_GFX. Arduino_GFX ships an Arduino_SH8601
// driver and an Arduino_ESP32QSPI bus; this is the same combination Waveshare's
// demos use for this panel.
//
// is_shared_interface = true acquires/releases the SPI bus lock per draw instead
// of holding it from begin() onwards, at the cost of one lock round-trip per
// flush chunk. It was REQUIRED while the SD card was a second device on this same
// SPI host; since 2026-07-24 the card runs on bit-banged software SPI (see
// sd_card.cpp) and nothing else touches SPI2, so this could now be false for a
// small flush speedup. Left true until someone re-verifies the panel on hardware.
static Arduino_DataBus *g_bus = new Arduino_ESP32QSPI(
    LCD_QSPI_CS, LCD_QSPI_SCK, LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3,
    true /* is_shared_interface */);
// Keep the typed SH8601 pointer so we can call its setBrightness(); g_gfx is
// the base-class view used for drawing.
// Arduino_GFX 1.6.x signature: (bus, rst, rotation, w, h, col/row offsets…).
// (Older versions had a `bool ips` before w/h; passing it here shifts w/h into
// the wrong slots and truncates the height into a uint8_t offset.)
static Arduino_SH8601 *g_amoled = new Arduino_SH8601(
    g_bus, LCD_RST_GPIO, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT);
static Arduino_GFX *g_gfx = g_amoled;

#elif BOARD_PANEL_QSPI_TFT
// AXS15231B IPS TFT over QSPI, matching Waveshare's own ESP32-S3-Touch-LCD-3.5B
// Arduino demos (08_gfx_helloworld, 09_lvgl_arduino_v8), which agree exactly:
//     Arduino_ESP32QSPI(CS, CLK, D0, D1, D2, D3)
//     Arduino_AXS15231B(bus, RST, rotation, ips, w, h)
//
// RST is -1 because the panel's reset is on the TCA9554 expander, not a GPIO;
// the pulse is issued over I2C in expanderPanelEnable() before begin() runs.
//
// `ips = false` is what the vendor passes. Note this is the OPPOSITE of the
// ST7796 path this replaced — do not "fix" it to true by analogy with the old
// plain-3.5 code, which was for a different panel on a different bus.
//
// is_shared_interface defaults to false, which is right here and is what the
// vendor relies on: the SD card is on the hardware SDMMC host, so nothing else
// touches this QSPI bus and the lock can be held from begin() onwards.
//
// INIT TABLE. We deliberately take Arduino_GFX's default rather than passing
// axs15231b_320480_type1/type2, despite this being a 320x480 panel. Waveshare's
// bundled copy of Arduino_GFX carries exactly ONE table, and it is byte-for-byte
// identical to upstream's axs15231b_180640_init_operations — which is the
// default. The "180640" in that name describes the panel the sequence was first
// written for, not the only geometry it drives; width and height are passed
// separately below. Verified by diffing the vendor library against libdeps
// 2026-08-10. If the panel ever comes up geometrically wrong rather than blank,
// the two 320480 tables are the things to try, passed as the 11th/12th args.
static Arduino_DataBus *g_bus = new Arduino_ESP32QSPI(
    LCD_QSPI_CS, LCD_QSPI_SCK, LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
static Arduino_GFX *g_gfx = new Arduino_AXS15231B(
    g_bus, LCD_RST_GPIO, 0 /* rotation */, false /* ips */, LCD_WIDTH, LCD_HEIGHT);
#else
#error "board_config.h defined no panel type"
#endif

namespace display {

// ---- LVGL draw buffer ------------------------------------------------------
// Each flush chunk pays a CASET/PASET/RAMWR overhead, so a taller buffer = fewer
// chunks = faster. How tall it can be is entirely a question of what memory the
// board has.
#if BOARD_PANEL_QSPI_TFT
// FULL frame per bank — this panel is not merely faster with a big buffer, it
// requires whole-frame writes to render correctly. See the full_refresh comment
// where the driver is registered. Two banks of 320*480*2 = 300 KB each, 600 KB
// total, which is 7 % of the 8 MB PSRAM.
static const uint32_t BUF_LINES = LCD_HEIGHT;
#elif BOARD_HAS_PSRAM
// 8 MB of PSRAM: take a third of the frame per bank, TWO banks (~300 KB total,
// under 4 % of PSRAM). LVGL then renders into one bank while the other is being
// flushed, and internal SRAM stays free for BLE and the rest of the system.
static const uint32_t BUF_LINES = LCD_HEIGHT / 3;
#else
// Partial buffer, ONE bank. AMOLED is 16bpp; a full framebuffer
// (368*448*2 = ~322 KB) is far too large for on-chip RAM.
//
// Sizing is a RAM tug-of-war: this board has 320 KB and no PSRAM, and NimBLE
// needs a ~25-30 KB contiguous block free to ESTABLISH a connection. With the UI
// this session grew (prefs, Wi-Fi/keyboard overlays, extra Settings cards) a
// 1/4-frame (112-line, 82 KB) buffer left only ~13 KB free / ~12 KB largest
// block, and BLE connects failed with HCI 0x3e. 1/7 (64 lines, ~47 KB) frees
// ~35 KB, restoring the headroom BLE needs while keeping redraws reasonable
// (7 flush chunks). If the UI ever slims back down, this can grow again — but
// keep a >=~30 KB contiguous margin for the BLE link, or connects break.
static const uint32_t BUF_LINES = LCD_HEIGHT / 7;
#endif
static lv_color_t *g_buf1 = nullptr;
static lv_color_t *g_buf2 = nullptr;
static lv_disp_draw_buf_t g_drawBuf;
static lv_disp_drv_t g_dispDrv;
static lv_indev_drv_t g_indevDrv;

static void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;
  g_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px, w, h);
  lv_disp_flush_ready(drv);
}


// ---- Capacitive touch (minimal I2C driver) ---------------------------------
// Two very different parts behind one signature.
// Returns: 1 = touch (x,y set), 0 = no touch, -1 = I2C read failed.
#if BOARD_TOUCH_FOCALTECH
static int readTouch(uint16_t &x, uint16_t &y) {
  // FT3168/FT6x36-family register map: 0x02 = touch count, 0x03.. = point data.
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(TOUCH_I2C_ADDR, 5) < 5) return -1;
  uint8_t touches = Wire.read() & 0x0F;
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if (touches == 0) return 0;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  // The register field is 12-bit; clamp glitched reads to the panel bounds so a
  // spike can't feed LVGL (or the snap engine) an off-screen point.
  if (x >= LCD_WIDTH) x = LCD_WIDTH - 1;
  if (y >= LCD_HEIGHT) y = LCD_HEIGHT - 1;
  return 1;
}

#elif BOARD_TOUCH_AXS15231B
// The AXS15231B's touch block is not a register file — there is no "read
// register 0x02". It answers a fixed 11-byte command with a 14-byte report, and
// that command is the only transaction the vendor driver ever issues
// (libraries/esp_lcd_touch_axs15231b/esp_lcd_touch_axs15231b.cpp, bsp_touch_read).
// The trailing 0x0e is the number of bytes it is being asked to return.
static const uint8_t AXS_READ_CMD[11] = {
    0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00};

// How long to wait between handing the controller its command and reading the
// answer back. The vendor reads immediately, but their bus runs slower, which
// gives the part the same thinking time by accident. A settle here buys that
// time explicitly without slowing the shared bus for the PMIC, RTC and codec.
#ifndef S3_TOUCH_SETTLE_US
#define S3_TOUCH_SETTLE_US 600
#endif

// Touch poll period, ms. 10 ms (the project default for the C6's register-file
// part) starves this one; see the note where the indev timer is set. Overridable
// from the build so the rate can be swept against the garbage-frame statistics
// below without editing code.
#ifndef S3_TOUCH_POLL_MS
#define S3_TOUCH_POLL_MS 20
#endif

static int readTouch(uint16_t &x, uint16_t &y) {
  uint8_t d[14];
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(AXS_READ_CMD, sizeof(AXS_READ_CMD));
  if (Wire.endTransmission() != 0) return -1;
#if S3_TOUCH_SETTLE_US > 0
  delayMicroseconds(S3_TOUCH_SETTLE_US);
#endif
  if (Wire.requestFrom((int)TOUCH_I2C_ADDR, (int)sizeof(d)) < (int)sizeof(d)) return -1;
  for (size_t i = 0; i < sizeof(d); i++) d[i] = Wire.read();

  // Frame-quality census, printed every 2 s when the debug flag is on. The
  // garbage rate is measurable with nobody touching the panel, which makes the
  // poll rate and settle time tunable without a human in the loop.
#if S3_TOUCH_FRAME_DEBUG
  {
    static uint32_t nIdle = 0, nGood = 0, nBad = 0, lastRep = 0;
    bool allZero = true;
    for (size_t i = 0; i < sizeof(d); i++) {
      if (d[i] != 0) { allZero = false; break; }
    }
    if (allZero || d[0] == 0xff) nIdle++;
    else if (d[1] > 2) nBad++;
    else nGood++;
    uint32_t now = millis();
    if (now - lastRep >= 2000) {
      lastRep = now;
      uint32_t tot = nIdle + nGood + nBad;
      Serial.printf("[tp] %lums/%luus  total=%lu idle=%lu good=%lu garbage=%lu (%lu%%)\n",
                    (unsigned long)S3_TOUCH_POLL_MS, (unsigned long)S3_TOUCH_SETTLE_US,
                    (unsigned long)tot, (unsigned long)nIdle, (unsigned long)nGood,
                    (unsigned long)nBad, (unsigned long)(tot ? nBad * 100 / tot : 0));
      nIdle = nGood = nBad = 0;
    }
  }
#endif

  // Opt-in frame dump. Off unless built with -D S3_TOUCH_FRAME_DEBUG=1, e.g.
  //     PLATFORMIO_BUILD_FLAGS="-D S3_TOUCH_FRAME_DEBUG=1" pio run -e ... -t upload
  // Prints every report so a slow drag can be read back as the sequence the
  // controller actually sends. This is how the frame layout documented below was
  // established, and how the 10 ms poll rate was caught starving it — worth
  // keeping, because nothing else here is observable from the outside.
#if S3_TOUCH_FRAME_DEBUG
  if (d[0] != 0xff) {
    Serial.printf("[tp] %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                  d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                  d[8], d[9], d[10], d[11], d[12], d[13]);
  } else {
    Serial.println("[tp] idle (ff)");
  }
#endif

  // Frame layout, confirmed by logging real reports off this board 2026-08-10:
  //
  //   idle          00 00 00 00 00 00 00 00 00 00 00 00 00 00
  //   one contact   00 01 80 97 00 cb 1c 1d ff ff ff ff ff ff
  //                 ^d0 ^count            ^-- unused
  //                       ^d2: event flag in the high nibble (0x80 = contact),
  //                           x's high nibble in the low one
  //                          ^d3: x low byte   -> x = 0x097 = 151
  //                             ^d4/^d5: same for y  -> y = 0x0cb = 203
  //
  // The vendor driver rejects frames on d[2]==0 / d[3]<2 / d[5]<2. Those tests
  // are not used here: d[2]==0 is simply a touch-DOWN at x<256, which is most of
  // a 320 px panel, and the other two throw away legitimate coordinates near an
  // edge. The observed frames make a cleaner split available — believe the
  // contact count, then range-check what it decodes to.
  static uint32_t holdingSinceMs = 0;

  // DEFINITE release. Zero contacts is the real idle report on this board (the
  // all-zero frame above); 0xff is the form the vendor documents. Accept both.
  if (d[0] == 0xff || d[1] == 0) {
    holdingSinceMs = 0;
    return 0;
  }

  // GARBAGE — hold the gesture rather than ending it. Two shapes show up: every
  // byte identical (0x12 repeated, ~6 % of reads) and frames shifted two bytes
  // out of alignment. Both decode to a contact count far above the two points
  // this part tracks. Reporting these as "no touch" would read to LVGL as
  // press-then-release mid-drag, turning a swipe into a click on whatever was
  // under the finger.
  //
  // The watchdog is the safety net the vendor driver lacks: holding forever
  // would strand a press if a release frame were ever missed, so after this long
  // with nothing believable, report the release anyway.
  const uint32_t HOLD_LIMIT_MS = 400;
  bool bad = (d[1] > 2);

  x = (uint16_t)(((d[2] & 0x0F) << 8) | d[3]);
  y = (uint16_t)(((d[4] & 0x0F) << 8) | d[5]);
  // A misaligned frame can still carry a plausible count, so range-check what it
  // actually decoded to before believing it.
  if (x >= LCD_WIDTH || y >= LCD_HEIGHT) bad = true;

  if (bad) {
    uint32_t now = millis();
    if (holdingSinceMs == 0) holdingSinceMs = now;
    if (now - holdingSinceMs > HOLD_LIMIT_MS) {
      holdingSinceMs = 0;
      return 0;
    }
    return -1;
  }
  holdingSinceMs = 0;
  // No clamp needed: an out-of-range decode was already routed to `bad` above,
  // so anything reaching here is on-panel by construction.
  return 1;
}
#else
#error "board_config.h defined no touch controller type"
#endif

// ---- Touch-target snapping ("guess what I meant") ---------------------------
// Compiled in only where the pixel density makes it worth it — see
// BOARD_TOUCH_SNAP in the board headers. On a low-DPI panel it does more harm
// than good, because snapping a press onto a slider or switch hands that control
// the whole drag and the list underneath can no longer be scrolled.
#if BOARD_TOUCH_SNAP
// The 1.8" panel is ~320 DPI, so even a 40 px button is barely 3 mm wide and a
// fingertip contact patch is 2-3x that — raw touch points frequently land just
// outside the intended control. On each NEW press we find the nearest
// interactive widget (a button, or a clickable object with a user event
// handler) within SNAP_RADIUS of the raw point. If the press missed it, the
// whole gesture is shifted by that miss offset, so press AND release land
// inside the guessed target. Because the offset is constant for the gesture,
// drag deltas are preserved and list/grid scrolling behaves exactly as before.
static const int32_t SNAP_RADIUS = 40;

// Overlays (menu / keypad / picker) live on the top layer and cover the whole
// screen; input must never snap through them to the screen underneath.
static lv_obj_t *snapRoot(lv_point_t p) {
  lv_obj_t *top = lv_layer_top();
  for (int i = (int)lv_obj_get_child_cnt(top) - 1; i >= 0; i--) {
    lv_obj_t *c = lv_obj_get_child(top, i);
    if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_area_t a;
    lv_obj_get_coords(c, &a);
    if (p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2) return c;
  }
  return lv_scr_act();
}

static bool isTapTarget(lv_obj_t *o) {
  if (!lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) return false;
  if (lv_obj_check_type(o, &lv_btn_class)) return true;
  // Plain containers are CLICKABLE by default; only ones somebody attached a
  // handler to (conn group, fault banner, ...) count as real targets.
  return o->spec_attr && o->spec_attr->event_dsc_cnt > 0;
}

static void nearestTarget(lv_obj_t *o, lv_point_t p, int32_t *bestD2, lv_obj_t **best) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
  if (isTapTarget(o)) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    // Clip to what is actually on-screen so we never snap to a row that is
    // scrolled out of view (area_is_visible truncates the rect to the visible
    // part and rejects fully clipped widgets).
    if (lv_obj_area_is_visible(o, &a)) {
      int32_t dx = p.x < a.x1 ? a.x1 - p.x : (p.x > a.x2 ? p.x - a.x2 : 0);
      int32_t dy = p.y < a.y1 ? a.y1 - p.y : (p.y > a.y2 ? p.y - a.y2 : 0);
      int32_t d2 = dx * dx + dy * dy;
      if (d2 < *bestD2) { *bestD2 = d2; *best = o; }
    }
  }
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++)
    nearestTarget(lv_obj_get_child(o, i), p, bestD2, best);
}

// Offset to add to this gesture so it lands on the guessed target (0,0 = the
// raw point already hit a target directly, or nothing is within snap range).
static lv_point_t snapOffset(lv_point_t p) {
  lv_point_t ofs = {0, 0};
  lv_obj_t *best = nullptr;
  int32_t bestD2 = SNAP_RADIUS * SNAP_RADIUS + 1;
  nearestTarget(snapRoot(p), p, &bestD2, &best);
  if (!best || bestD2 == 0) return ofs;
  lv_area_t a;
  lv_obj_get_coords(best, &a);
  lv_obj_area_is_visible(best, &a);
  ofs.x = (lv_coord_t)(LV_CLAMP(a.x1 + 3, p.x, a.x2 - 3) - p.x);
  ofs.y = (lv_coord_t)(LV_CLAMP(a.y1 + 3, p.y, a.y2 - 3) - p.y);
  return ofs;
}
#endif  // BOARD_TOUCH_SNAP

// Defined fully in the Display-sleep section below; declared here because the
// touch callback (above that section) must know when the panel is blanked.
static bool g_asleep = false;

static void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
  // Hold the last reported state on a transient I2C failure: reporting a spurious
  // RELEASE mid-press makes LVGL drop the click and taps feel unresponsive.
  static lv_indev_state_t lastState = LV_INDEV_STATE_REL;
  static lv_point_t lastPt = {0, 0};
#if BOARD_TOUCH_SNAP
  static lv_point_t snapOfs = {0, 0};
#endif
  uint16_t x, y;
  int r = readTouch(x, y);
  // While the panel is blanked, a tap must WAKE rather than act: the UI is
  // invisible, so letting the press through would blind-press whatever sits
  // under the finger. Report a release and swallow the gesture.
  if (g_asleep) {
    if (r == 1) noteActivity();   // wakes; the gesture itself is swallowed
    data->state = LV_INDEV_STATE_REL;
    data->point = lastPt;
    return;
  }
  if (r == 1) {
    noteActivity();   // also undoes an idle dim before the tap is delivered
    lv_point_t p = {(lv_coord_t)x, (lv_coord_t)y};
#if BOARD_TOUCH_SNAP
    if (lastState == LV_INDEV_STATE_REL) snapOfs = snapOffset(p);  // new gesture
    lastPt.x = (lv_coord_t)(p.x + snapOfs.x);
    lastPt.y = (lv_coord_t)(p.y + snapOfs.y);
#else
    lastPt = p;   // report the finger where it actually is
#endif
    lastState = LV_INDEV_STATE_PR;
  } else if (r == 0) {
    lastState = LV_INDEV_STATE_REL;
  }
  // r == -1: keep lastState/lastPt unchanged.
  data->state = lastState;
  data->point = lastPt;
}

#if BOARD_TOUCH_AXS15231B
// Nothing to do. The AXS15231B's touch block has no power-mode or auto-sleep
// registers to write — the vendor's bsp_touch_init() only stores the geometry
// and, on boards that wire one, pulses a reset GPIO. This board does not wire
// one (tp_rst = -1), and the controller shares the panel's reset, which
// expanderPanelEnable() has already pulsed by the time we get here.
//
// Do NOT be tempted to send the FocalTech register writes below "just in case":
// they are not no-ops on this part. It parses whatever arrives as the start of a
// command frame, and a stray 2-byte write leaves it out of sync with the 11-byte
// command the read path issues.
static void touchInit() {}

#elif BOARD_TOUCH_FOCALTECH
// The FT3168 powers up in a low-power state and stops updating its touch
// registers (0x02 stays 0) until its power mode is configured — Waveshare's
// driver writes register 0xA5 during init. We set Active mode (continuous scan)
// so a polled driver always sees the current touch state (0x00=Active,
// 0x01=Monitor). Without this, touches never register.
static void touchInit() {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0xA5);
  Wire.write(0x00);  // Active mode
  Wire.endTransmission();
  // Disable the controller's idle auto-sleep. Left enabled, the chip drops into
  // a slow scan after ~10 s without a touch, so the first short tap after an
  // idle period falls between scans and never registers — taps feel dead until
  // a second or third attempt wakes it.
  //
  // Waveshare document this panel's touch as CST820 (Hynitron) while the
  // register map it answers on is FocalTech-compatible (a live dump read
  // CTRL=1, TIMEENTERMONITOR=10, PERIODACTIVE=8, PERIODMONITOR=50 — textbook
  // FocalTech values). Board revisions differ, so write BOTH families' idle
  // registers: each is a harmless no-op on the other part.
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0x86);
  Wire.write(0x00);  // FocalTech CTRL: stay Active, never auto-enter Monitor
  Wire.endTransmission();
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0xFE);
  Wire.write(0x01);  // CST8xx DisAutoSleep: non-zero disables auto-sleep
  Wire.endTransmission();
  delay(20);
}
#endif

static uint8_t g_brightness = LCD_DEFAULT_BRIGHTNESS;

// Push a 0..255 level at the panel. The two boards do this in completely
// different ways — a controller command on the AMOLED, a PWM'd backlight rail on
// the TFT — so everything else in this file goes through here and never touches
// the panel directly.
static void applyBrightness(uint8_t level) {
#if BOARD_BACKLIGHT_PWM
  // LEDC PWM on the backlight pin. Scale the 0..255 UI range onto the timer's
  // full duty range so the top of the slider is genuinely full brightness.
  const uint32_t maxDuty = (1u << LCD_BL_PWM_BITS) - 1u;
  ledcWrite(LCD_BL_GPIO, (uint32_t)level * maxDuty / 255u);
#else
  // Arduino_SH8601 exposes setBrightness() (SH8601 command 0x51). If your
  // Arduino_GFX version predates it, update the library or send 0x51 via the
  // bus here.
  g_amoled->setBrightness(level);
#endif
}

void setBrightness(uint8_t level) {
  g_brightness = level;
  applyBrightness(level);
}

uint8_t getBrightness() { return g_brightness; }

// ---- System info (AXP2101 PMIC + PCF85063 RTC on the shared I2C bus) --------
static int i2cReadReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)addr, 1) < 1) return -1;
  return Wire.read();
}

static bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool batteryStats(int &percent, int &milliVolt, int &chargeState, bool &present) {
  int st0 = i2cReadReg(PMIC_I2C_ADDR, 0x00);
  if (st0 < 0) return false;
  present = (st0 & 0x08) != 0;               // STATUS1 bit3: battery-present flag
  int p = i2cReadReg(PMIC_I2C_ADDR, 0xA4);   // fuel-gauge percent
  int h = i2cReadReg(PMIC_I2C_ADDR, 0x34);   // VBAT ADC high (6 bits)
  int l = i2cReadReg(PMIC_I2C_ADDR, 0x35);   // VBAT ADC low
  int s1 = i2cReadReg(PMIC_I2C_ADDR, 0x01);  // charger status in low 3 bits
  percent = p < 0 ? 0 : p;
  milliVolt = (h < 0 || l < 0) ? 0 : (((h & 0x3F) << 8) | l);
  chargeState = s1 < 0 ? -1 : (s1 & 0x07);
  return true;
}

// USB / VBUS present = the controller is on wall power (AXP2101 STATUS1 bit5,
// "VBUS good"). On USB there is no brownout risk, so the power monitor uses this
// to suppress the low-battery-force-off path.
bool usbPresent() {
  int st0 = i2cReadReg(PMIC_I2C_ADDR, 0x00);
  return st0 >= 0 && (st0 & (1 << 5)) != 0;
}

// Clean hardware power-off via the AXP2101 (COMMON_CONFIG 0x10 bit0 = shutdown:
// cuts every rail). Read-modify-write so the other config bits are preserved.
// The caller MUST have already forced the EL15 load off — this kills our own
// power, and anything still sinking current would be stranded. Returns only if
// the PMIC did not power us down (then the caller falls back to a reset).
void powerOff() {
  int cc = i2cReadReg(PMIC_I2C_ADDR, 0x10);
  if (cc < 0) return;
  i2cWriteReg(PMIC_I2C_ADDR, 0x10, (uint8_t)(cc | 0x01));
}

// ---- Display sleep ---------------------------------------------------------
// g_asleep is declared up by touchReadCb (which needs it); defined there.
static uint8_t g_wakeBrightness = LCD_DEFAULT_BRIGHTNESS;

bool asleep() { return g_asleep; }

void setSleep(bool on) {
  if (on == g_asleep) return;
  g_asleep = on;
  if (on) {
    g_wakeBrightness = g_brightness;
    applyBrightness(0);   // keep g_brightness as the restore value
  } else {
    setBrightness(g_wakeBrightness);
  }
}

// ---- Physical buttons ------------------------------------------------------
ButtonEvent pollButtons() {
  uint32_t now = millis();

  // Startup settle window: ignore (but keep clearing) button state for the
  // first second so a power-on PKEY latch or BOOT-pin bounce can't fire an
  // action against a UI that has only just come up.
  if (now < 1500) {
    Wire.beginTransmission(PMIC_I2C_ADDR);
    Wire.write(PMIC_REG_INTSTS2);
    Wire.write((uint8_t)(PMIC_IRQ_PKEY_SHORT | PMIC_IRQ_PKEY_LONG));
    Wire.endTransmission();
    return BTN_NONE;
  }

  // BOOT (GPIO, active low): debounce, then classify on release so a long
  // press can be distinguished without firing the short action first.
  static bool bootDown = false;
  static uint32_t bootEdgeMs = 0;
  static bool bootLongFired = false;
  bool bootRaw = digitalRead(BOOT_BTN_GPIO) == LOW;
  if (bootRaw != bootDown && (now - bootEdgeMs) > BTN_DEBOUNCE_MS) {
    bootDown = bootRaw;
    bootEdgeMs = now;
    if (bootDown) {
      bootLongFired = false;
    } else if (!bootLongFired) {
      return BTN_BOOT_SHORT;
    }
  }
  if (bootDown && !bootLongFired && (now - bootEdgeMs) >= BTN_LONG_PRESS_MS) {
    bootLongFired = true;   // fire while still held; release is then swallowed
    return BTN_BOOT_LONG;
  }

  // PWR: latched IRQ bits in the PMIC, polled at 50 ms so we don't hammer the
  // shared I2C bus that touch also lives on.
  static uint32_t lastPmicMs = 0;
  if (now - lastPmicMs >= 50) {
    lastPmicMs = now;
    int sts = i2cReadReg(PMIC_I2C_ADDR, PMIC_REG_INTSTS2);
    if (sts > 0 && (sts & (PMIC_IRQ_PKEY_SHORT | PMIC_IRQ_PKEY_LONG))) {
      // Write the bits back to clear them (write-1-to-clear).
      Wire.beginTransmission(PMIC_I2C_ADDR);
      Wire.write(PMIC_REG_INTSTS2);
      Wire.write((uint8_t)(sts & (PMIC_IRQ_PKEY_SHORT | PMIC_IRQ_PKEY_LONG)));
      Wire.endTransmission();
      if (sts & PMIC_IRQ_PKEY_LONG) return BTN_PWR_LONG;
      return BTN_PWR_SHORT;
    }
  }
  return BTN_NONE;
}

bool rtcTime(int &year, int &mon, int &day, int &hour, int &min, int &sec) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(0x04);  // seconds register, then min/hour/day/weekday/month/year
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)RTC_I2C_ADDR, 7) < 7) return false;
  uint8_t r[7];
  for (int i = 0; i < 7; i++) r[i] = Wire.read();
  if (r[0] & 0x80) return false;  // OS flag set: oscillator stopped, time invalid
  auto bcd = [](uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); };
  sec = bcd(r[0] & 0x7F);
  min = bcd(r[1] & 0x7F);
  hour = bcd(r[2] & 0x3F);
  day = bcd(r[3] & 0x3F);
  mon = bcd(r[5] & 0x1F);
  year = 2000 + bcd(r[6]);
  return true;
}

bool setRtcTime(int year, int mon, int day, int hour, int min, int sec) {
  if (year < 2000 || year > 2099) return false;
  auto bcd = [](int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
  // Control_1 bit 5 (STOP) must be clear for the oscillator to run; clear it
  // first in case the part came up halted, then write the time. Writing the
  // seconds register also clears the OS (oscillator-stop) flag, which is what
  // rtcTime() reads to decide whether the clock has ever been set.
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(0x00);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(0x04);          // auto-increments through 0x0A
  Wire.write(bcd(sec) & 0x7F);
  Wire.write(bcd(min));
  Wire.write(bcd(hour));     // 24-hour mode (Control_1 bit 1 left at 0)
  Wire.write(bcd(day));
  Wire.write(0);             // weekday: unused by rtcTime()
  Wire.write(bcd(mon));
  Wire.write(bcd(year - 2000));
  return Wire.endTransmission() == 0;
}

// Both boards put the panel's reset/enable behind the TCA9554 I/O expander at
// 0x20 rather than on a GPIO, so the panel must be brought up over I2C BEFORE
// the controller is initialised. What "brought up" means differs:
//
//   C6 AMOLED : hold expander bits 4 & 5 HIGH — they power the panel rail and
//               hold it out of reset for as long as they stay asserted.
//   S3 TFT    : PULSE expander bit 1 low then high — it is an ordinary
//               active-low reset line, so it is asserted momentarily and
//               released, not held.
static void expanderPanelEnable() {
#if LCD_RST_VIA_EXPANDER
  auto rmw = [&](uint8_t reg, uint8_t setMask, uint8_t clrMask) {
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return;
    Wire.requestFrom((int)IO_EXPANDER_ADDR, 1);
    uint8_t v = Wire.available() ? Wire.read() : 0x00;
    v = (uint8_t)((v & ~clrMask) | setMask);
    Wire.beginTransmission(IO_EXPANDER_ADDR);
    Wire.write(reg);
    Wire.write(v);
    Wire.endTransmission();
  };
#if LCD_RST_EXPANDER_ACTIVE_LOW
  const uint8_t bit = LCD_EXPANDER_RST_BIT;
  rmw(0x03, 0x00, bit);   // config reg: 0 = output
  rmw(0x01, bit, 0x00);   // start released (high)
  delay(10);
  rmw(0x01, 0x00, bit);   // assert reset (low)
  delay(10);
  rmw(0x01, bit, 0x00);   // release
  delay(200);             // ST7796 needs ~120 ms after reset before commands
#else
  const uint8_t bits = LCD_EXPANDER_PWR_BITS;
  rmw(0x03, 0x00, bits);  // config reg: 0 = output → make bits 4,5 outputs
  rmw(0x01, bits, 0x00);  // output reg: drive bits 4,5 high
  delay(500);             // let the panel power rail / reset settle
#endif
#endif
}

// One-shot census of the shared I2C bus, printed at boot before anything is
// asked to work. Every peripheral on both boards except the panel itself hangs
// off this one bus, so "which addresses answered" is the first question a
// bring-up asks and the cheapest one to answer. A part that is missing here
// explains every later failure that touches it — without having to infer it from
// a flood of per-transaction errors, which is how the S3 board's touch
// controller was first found (2026-08-10).
//
// endTransmission() with an empty buffer sends the address alone and reports the
// ACK, and unlike requestFrom() it does not log on NACK, so the scan is quiet
// for the ~110 addresses that are not populated.
static void i2cScan() {
  Serial.print("[i2c] scan:");
  int found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", addr);
      found++;
    }
  }
  if (found == 0) Serial.print(" nothing responded — check SDA/SCL wiring");
  Serial.printf("  (%d device%s)\n", found, found == 1 ? "" : "s");
}

void begin() {
  // I2C first: the panel reset/enable is on the TCA9554 expander, so the panel
  // must be released from reset over I2C *before* the SH8601 bring-up.
  Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
  Wire.setClock(400000);
  // Scan BEFORE the expander/touch writes below, so what prints is the bus as
  // found rather than the bus as this function has already poked it.
  i2cScan();
  expanderPanelEnable();
  touchInit();

  // Enable the AXP2101's VBAT ADC channel (reg 0x30 bit 0, off by default) so
  // the Settings screen can read the battery voltage.
  int adc = i2cReadReg(PMIC_I2C_ADDR, 0x30);
  if (adc >= 0) {
    Wire.beginTransmission(PMIC_I2C_ADDR);
    Wire.write(0x30);
    Wire.write((uint8_t)(adc | 0x01));
    Wire.endTransmission();
  }

  // Enable the AXP2101 PWRKEY short/long-press IRQs so pollButtons() can read
  // PWR-key presses (the key is on the PMIC, not a GPIO).
  int inten2 = i2cReadReg(PMIC_I2C_ADDR, PMIC_REG_INTEN2);
  if (inten2 >= 0) {
    Wire.beginTransmission(PMIC_I2C_ADDR);
    Wire.write(PMIC_REG_INTEN2);
    Wire.write((uint8_t)(inten2 | PMIC_IRQ_PKEY_SHORT | PMIC_IRQ_PKEY_LONG));
    Wire.endTransmission();
  }
  // Clear any stale latched PKEY bits UNCONDITIONALLY — power-on itself latches
  // a PKEY-short IRQ, and if the enable-read above hiccupped on the shared I2C
  // bus we'd otherwise read that stale bit on the first poll and fire a phantom
  // emergency stop. (A startup settle window in pollButtons() backs this up.)
  Wire.beginTransmission(PMIC_I2C_ADDR);
  Wire.write(PMIC_REG_INTSTS2);
  Wire.write((uint8_t)(PMIC_IRQ_PKEY_SHORT | PMIC_IRQ_PKEY_LONG));
  Wire.endTransmission();

  // BOOT button: strapping pin, has an external pull-up; INPUT is enough.
  pinMode(BOOT_BTN_GPIO, INPUT_PULLUP);

#if BOARD_BACKLIGHT_PWM
  // Bring the backlight up dark and let setBrightness() below raise it, so the
  // panel's first uninitialised frame is never shown.
  //
  // Report the attach result: if LEDC refuses the pin/frequency/resolution
  // combination, ledcWrite() silently does nothing and the backlight sits at
  // whatever the pin floats to. That presents as "the panel is too dim", which
  // is indistinguishable by eye from a genuinely underdriven backlight — so make
  // the difference readable in the boot log instead of guessable.
  if (!ledcAttach(LCD_BL_GPIO, LCD_BL_PWM_HZ, LCD_BL_PWM_BITS)) {
    Serial.printf("[display] backlight LEDC attach FAILED on GPIO %d "
                  "(%d Hz, %d-bit) — brightness control is dead\n",
                  (int)LCD_BL_GPIO, (int)LCD_BL_PWM_HZ, (int)LCD_BL_PWM_BITS);
  }
  ledcWrite(LCD_BL_GPIO, 0);
#endif

  // Pixel clock: see LCD_SPI_HZ in the board header for why each board uses the
  // value it does. The synchronous flush dominates every redraw, so this is the
  // single biggest lever on screen-transition speed.
  if (!g_gfx->begin(LCD_SPI_HZ)) {
    // Panel bring-up failed — most often a bus pin mismatch. Check board_config.h.
    Serial.println("[display] Arduino_GFX begin() failed — verify panel bus pins");
  }
  g_gfx->fillScreen(RGB565_BLACK);
  setBrightness(LCD_DEFAULT_BRIGHTNESS);

  lv_init();
  size_t bufPx = (size_t)LCD_WIDTH * BUF_LINES;
#if BOARD_HAS_PSRAM
  // With 8 MB of PSRAM the RAM tug-of-war that shapes the C6 build does not
  // exist: take two big buffers so LVGL can render the next chunk while the
  // current one is being pushed, and leave internal RAM entirely to BLE and the
  // rest of the system. PSRAM is slower than internal SRAM, but these buffers are
  // written once and read once per flush, which is exactly the access pattern it
  // handles well.
  g_buf1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  g_buf2 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (!g_buf1) {
    // Falling back rather than dying: a board whose PSRAM did not come up (wrong
    // memory_type in platformio.ini is the usual cause) still boots to a usable
    // UI on a smaller internal buffer, and says so.
    Serial.println("[display] PSRAM draw-buffer alloc FAILED — is memory_type qio_opi? "
                   "falling back to internal RAM");
    bufPx = (size_t)LCD_WIDTH * (LCD_HEIGHT / 8);
    g_buf1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t),
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    g_buf2 = nullptr;
  }
#else
  g_buf1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#endif
  if (!g_buf1) {
    Serial.println("[display] LVGL draw-buffer alloc failed — reduce BUF_LINES");
    return;  // don't hand LVGL null buffers
  }
  lv_disp_draw_buf_init(&g_drawBuf, g_buf1, g_buf2, bufPx);

  lv_disp_drv_init(&g_dispDrv);
  g_dispDrv.hor_res = LCD_WIDTH;
  g_dispDrv.ver_res = LCD_HEIGHT;
  g_dispDrv.flush_cb = flushCb;
  g_dispDrv.draw_buf = &g_drawBuf;
#if BOARD_PANEL_QSPI_TFT
  // Whole-frame writes ONLY. The AXS15231B does not render a partial write
  // correctly on this board: a flush that does not start at row 0 and span the
  // full width walks out of step with the controller's line stride, and every
  // line after the first lands offset from the one above.
  //
  // Observed on the bench 2026-08-10: with ordinary partial flushes the first
  // paint was recognisable but every other line was shifted, and tapping a
  // widget — which repaints a small region — produced garbage. Rounding the
  // flush out to full WIDTH alone did not help, which is what rules out a
  // column-alignment quirk and points at the row start.
  //
  // Waveshare's own demos never encounter this because they drive the panel
  // through an Arduino_Canvas and push the entire framebuffer on every flush;
  // there is no partial write anywhere in their code. full_refresh = 1 is the
  // LVGL equivalent: LVGL renders the whole screen into one buffer and hands it
  // over in a single flush spanning (0,0)-(W-1,H-1), so draw16bitRGBBitmap sees
  // exactly the one full-frame write the vendor issues.
  //
  // The cost is redrawing every pixel on every frame instead of just the dirty
  // region. That is affordable here and nowhere else in this codebase: 300 KB
  // per frame over an 80 MHz quad-lane bus is ~8 ms, the buffers live in PSRAM
  // where there is room for two, and there is no contiguous-internal-heap
  // pressure on this board of the kind that shapes the C6 build.
  g_dispDrv.full_refresh = 1;
#endif
  lv_disp_drv_register(&g_dispDrv);

  lv_indev_drv_init(&g_indevDrv);
  g_indevDrv.type = LV_INDEV_TYPE_POINTER;
  g_indevDrv.read_cb = touchReadCb;
  lv_indev_t *indev = lv_indev_drv_register(&g_indevDrv);

#if BOARD_TOUCH_AXS15231B
  // Poll this controller at 30 ms, not the 10 ms the rest of the project uses.
  //
  // lv_conf.h sets LV_INDEV_DEF_READ_PERIOD to 10, which suits the C6's
  // FocalTech part: that one is a register file, and reading it is a cheap,
  // stateless transaction you can repeat as fast as you like. The AXS15231B is
  // not — every poll is an 11-byte command followed by a 14-byte response, and
  // it needs time to service that.
  //
  // Measured on the bench 2026-08-10 at 10 ms: of 6704 consecutive reads, 6097
  // came back all-zero and 389 came back as pure garbage (every byte 0x12, plus
  // some frames shifted 2 bytes out of alignment). Only about ten were real
  // touch reports. That is enough for a tap, which needs one good frame, and
  // nowhere near enough for a drag, which needs a continuous stream of moving
  // coordinates — which is exactly how it behaved: taps fine, scrolling
  // impossible.
  //
  // 30 ms is what Waveshare's own LVGL example runs (their lv_conf.h sets
  // LV_INDEV_DEF_READ_PERIOD 30), i.e. the rate this part is actually known to
  // work at. Set per-board here rather than in lv_conf.h so the C6 keeps its
  // 10 ms and the snappier touch feel that goes with it.
  if (indev && indev->driver && indev->driver->read_timer) {
    lv_timer_set_period(indev->driver->read_timer, S3_TOUCH_POLL_MS);
  }
#else
  (void)indev;
#endif
}

// ---- Screen care -----------------------------------------------------------
// This is a static instrument UI: "VOLTAGE", "CURRENT", the status strip and the
// card outlines sit in exactly the same pixels for hours at a time. Two cheap
// defences — but only the second applies to both boards:
//
//   1. Pixel shift — the whole UI creeps around a 3x3 px box, one step every
//      PIXEL_SHIFT_MS. No edge sits over the same subpixel for more than a few
//      minutes, which is what smears the wear out. Applied as a translate on
//      the active screen and the overlay layer, so it costs one redraw per step
//      and needs no cooperation from the UI code.
//      **AMOLED ONLY.** On an OLED a static UI is permanent damage rather than a
//      temporary artefact, which is what makes the constant creeping worth a
//      redraw every 90 s. An IPS LCD does not burn in, so on that board this
//      defaults OFF and the redraws are pure waste. The setting still exists
//      there (a user can switch it on), it simply starts disabled.
//   2. Idle dim, then black — after idleDimS of no touch the panel drops to a
//      readable-but-dark level; after IDLE_SLEEP_FACTOR x that it blanks
//      entirely. This saves real power on BOTH boards, for different reasons: an
//      AMOLED drawing black lights no pixels, and on the TFT the blank state
//      drives the backlight PWM to zero, which is where nearly all of that
//      board's display power goes. Either state ends on the first touch. Sleep
//      is suppressed while inhibitSleep(true) is set, so a long unattended test
//      keeps its (dimmed) screen instead of going dark mid-run.
static const uint32_t PIXEL_SHIFT_MS = 90000;
static const int IDLE_SLEEP_FACTOR = 5;
static const uint8_t IDLE_DIM_LEVEL = 24;   // out of 255

static bool g_pixelShift = BOARD_PANEL_QSPI_AMOLED ? true : false;
static uint16_t g_idleDimS = 0;             // 0 = never dim
static bool g_inhibitSleep = false;
static bool g_dimmed = false;
static uint32_t g_lastActivityMs = 0;

void setPixelShift(bool on) {
  g_pixelShift = on;
  if (!on) {   // park the layout back at its true position
    lv_obj_set_style_translate_x(lv_scr_act(), 0, 0);
    lv_obj_set_style_translate_y(lv_scr_act(), 0, 0);
    lv_obj_set_style_translate_x(lv_layer_top(), 0, 0);
    lv_obj_set_style_translate_y(lv_layer_top(), 0, 0);
  }
}
bool pixelShift() { return g_pixelShift; }

void setIdleDim(uint16_t seconds) {
  g_idleDimS = seconds;
  noteActivity();
}
uint16_t idleDim() { return g_idleDimS; }

void inhibitSleep(bool on) { g_inhibitSleep = on; }

void noteActivity() {
  g_lastActivityMs = millis();
  if (g_dimmed) {   // restore the user's brightness on any interaction
    g_dimmed = false;
    setBrightness(g_brightness);
  }
  if (g_asleep) setSleep(false);
}

static void pixelShiftTick() {
  static const lv_coord_t OFS[][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
  static uint32_t lastMs = 0;
  static uint8_t idx = 0;
  if (!g_pixelShift) return;
  uint32_t now = millis();
  if (now - lastMs < PIXEL_SHIFT_MS) return;
  lastMs = now;
  idx = (uint8_t)((idx + 1) % (sizeof(OFS) / sizeof(OFS[0])));
  lv_obj_t *targets[2] = {lv_scr_act(), lv_layer_top()};
  for (lv_obj_t *t : targets) {
    lv_obj_set_style_translate_x(t, OFS[idx][0], 0);
    lv_obj_set_style_translate_y(t, OFS[idx][1], 0);
  }
}

static void idleTick() {
  if (g_idleDimS == 0 || g_asleep) return;
  uint32_t idleMs = millis() - g_lastActivityMs;
  if (!g_dimmed && idleMs >= (uint32_t)g_idleDimS * 1000) {
    g_dimmed = true;
    // Straight to the panel: g_brightness stays the user's value so any wake
    // (or a Settings change) restores it without having to remember it here.
    applyBrightness(min(IDLE_DIM_LEVEL, g_brightness));
  }
  if (g_dimmed && !g_inhibitSleep &&
      idleMs >= (uint32_t)g_idleDimS * IDLE_SLEEP_FACTOR * 1000) {
    setSleep(true);
  }
}

// ---- Low-memory mode (Wi-Fi window) ----------------------------------------
// This board has 320 KB of RAM and no PSRAM. Even with the draw buffer trimmed
// to fit the BLE stack (see BUF_LINES), the ~37 KB free is nowhere near the
// ~50 KB esp_wifi_init wants, so a Wi-Fi scan/NTP sync would fail with NO_MEM.
// Rather than tear BLE down (which would strand a live load), we shrink the draw
// buffer for the duration of a Wi-Fi op: the ~47 KB buffer is swapped for a tiny
// one, freeing enough for Wi-Fi while the UI keeps rendering (just in more,
// smaller flush chunks — slower, fine for a settings screen). Restored, best
// effort, the moment Wi-Fi is done (a successful NTP sync reboots to fully
// reclaim the buffer — see onNetProgress).
static bool g_lowMem = false;

#if !BOARD_HAS_PSRAM
static const uint32_t SMALL_BUF_LINES = 16;   // ~11.5 KB, frees the most for Wi-Fi

static bool allocDrawBuf(uint32_t lines) {
  size_t bufPx = (size_t)LCD_WIDTH * lines;
  lv_color_t *nb = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t),
                                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!nb) return false;
  g_buf1 = nb;
  lv_disp_draw_buf_init(&g_drawBuf, g_buf1, nullptr, bufPx);
  return true;
}
#endif

// Swap the draw buffer. Frees the current one FIRST so the replacement (and, in
// low-mem mode, Wi-Fi) has room — we only have ~1.5 KB spare, nowhere near
// enough to hold both buffers at once. Safe because we run on the loop task and
// never from inside flushCb.
//
// Restoring is best-effort: once Wi-Fi has churned the heap, the original 82 KB
// contiguous block usually can't be reassembled (largest free block ~18-40 KB),
// so we probe DOWN from full and take the biggest buffer that still allocs. The
// UI ends up as fast as the fragmented heap allows and fully recovers on the
// next reboot. Entering low-mem always uses the smallest buffer to free the
// most for Wi-Fi.
void setLowMemMode(bool on) {
#if BOARD_HAS_PSRAM
  // Nothing to do on a board with PSRAM, and doing it anyway would make things
  // WORSE: the draw buffers live in PSRAM here, so internal RAM is already free
  // for Wi-Fi, while allocDrawBuf() allocates MALLOC_CAP_INTERNAL — the "shrink"
  // would move the buffer OUT of PSRAM and INTO the very memory Wi-Fi wants, and
  // drop the second bank on the way. The callers stay unchanged (the UI still
  // announces a Wi-Fi window); this simply becomes the no-op it should be.
  (void)on;
  return;
#else
  if (on == g_lowMem) return;
  heap_caps_free(g_buf1);
  g_buf1 = nullptr;
  uint32_t got = 0;
  if (on) {
    got = allocDrawBuf(SMALL_BUF_LINES) ? SMALL_BUF_LINES : 0;
  } else {
    // Largest-first ladder, capped at the build-time full size.
    static const uint32_t LADDER[] = {BUF_LINES, 96, 80, 64, 48, 32, 24, SMALL_BUF_LINES};
    for (uint32_t lines : LADDER) {
      if (lines > BUF_LINES) continue;
      if (allocDrawBuf(lines)) { got = lines; break; }
    }
  }
  if (!got) {
    Serial.println("[display] FATAL: no draw buffer after swap");
    return;   // next flush would deref null; nothing safe to do here
  }
  g_lowMem = got < BUF_LINES;
  lv_obj_invalidate(lv_scr_act());
  lv_obj_invalidate(lv_layer_top());
  lv_refr_now(NULL);
  Serial.printf("[display] draw buffer -> %u lines (%s), heap now %u free\n",
                (unsigned)got, g_lowMem ? "reduced" : "full",
                (unsigned)ESP.getFreeHeap());
#endif
}

bool lowMemMode() { return g_lowMem; }

void loopTick() {
  lv_timer_handler();
  pixelShiftTick();
  idleTick();
}

}  // namespace display
