#include "display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <esp_heap_caps.h>

#include "board_config.h"

// SH8601 AMOLED over QSPI via Arduino_GFX. Arduino_GFX ships an Arduino_SH8601
// driver and an Arduino_ESP32QSPI bus; this is the same combination Waveshare's
// demos use for this panel.
//
// is_shared_interface = true is REQUIRED, not cosmetic: with it false the
// driver acquires the SPI bus lock once in begin() and never releases it, which
// would block the SD card (a second device on the C6's only SPI host — see
// sd_card.cpp) forever. Shared mode acquires/releases per draw instead, which
// costs one lock round-trip per flush chunk.
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

namespace display {

// ---- LVGL draw buffer ------------------------------------------------------
// Partial buffer, ONE bank. AMOLED is 16bpp; a full framebuffer
// (368*448*2 = ~322 KB) is far too large for on-chip RAM, and each flush chunk
// pays a CASET/PASET/RAMWR overhead, so a taller buffer = fewer chunks = faster.
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
static lv_color_t *g_buf1 = nullptr;
static lv_disp_draw_buf_t g_drawBuf;
static lv_disp_drv_t g_dispDrv;
static lv_indev_drv_t g_indevDrv;

static void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;
  g_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px, w, h);
  lv_disp_flush_ready(drv);
}

// ---- FT3168 capacitive touch (minimal I2C driver) --------------------------
// Returns: 1 = touch (x,y set), 0 = no touch, -1 = I2C read failed.
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

// ---- Touch-target snapping ("guess what I meant") ---------------------------
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

// Defined fully in the Display-sleep section below; declared here because the
// touch callback (above that section) must know when the panel is blanked.
static bool g_asleep = false;

static void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
  // Hold the last reported state on a transient I2C failure: reporting a spurious
  // RELEASE mid-press makes LVGL drop the click and taps feel unresponsive.
  static lv_indev_state_t lastState = LV_INDEV_STATE_REL;
  static lv_point_t lastPt = {0, 0};
  static lv_point_t snapOfs = {0, 0};
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
    if (lastState == LV_INDEV_STATE_REL) snapOfs = snapOffset(p);  // new gesture
    lastPt.x = (lv_coord_t)(p.x + snapOfs.x);
    lastPt.y = (lv_coord_t)(p.y + snapOfs.y);
    lastState = LV_INDEV_STATE_PR;
  } else if (r == 0) {
    lastState = LV_INDEV_STATE_REL;
  }
  // r == -1: keep lastState/lastPt unchanged.
  data->state = lastState;
  data->point = lastPt;
}

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

static uint8_t g_brightness = LCD_DEFAULT_BRIGHTNESS;

void setBrightness(uint8_t level) {
  // Arduino_SH8601 exposes setBrightness() (SH8601 command 0x51). If your
  // Arduino_GFX version predates it, update the library or send 0x51 via the
  // bus here.
  g_brightness = level;
  g_amoled->setBrightness(level);
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
    g_amoled->setBrightness(0);   // keep g_brightness as the restore value
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

// The AMOLED's reset/enable lines are on the on-board TCA9554 I/O expander, not
// a GPIO. Drive expander bits 4 & 5 high (read-modify-write on the config +
// output registers) to power/enable the panel and release it from reset, then
// let it settle — exactly what Waveshare's demo does before gfx->begin().
static void expanderPanelEnable() {
#if LCD_RST_VIA_EXPANDER
  const uint8_t bits = LCD_EXPANDER_PWR_BITS;
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
  rmw(0x03, 0x00, bits);  // config reg: 0 = output → make bits 4,5 outputs
  rmw(0x01, bits, 0x00);  // output reg: drive bits 4,5 high
  delay(500);             // let the panel power rail / reset settle
#endif
}

void begin() {
  // I2C first: the panel reset/enable is on the TCA9554 expander, so the panel
  // must be released from reset over I2C *before* the SH8601 bring-up.
  Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
  Wire.setClock(400000);
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

  // Run the QSPI display bus at 80 MHz (the SH8601/CO5300 handle it) instead of
  // Arduino_GFX's 40 MHz default — the synchronous flush dominates every redraw,
  // so doubling the pixel clock roughly halves screen-transition time.
  if (!g_gfx->begin(80000000)) {
    // Panel bring-up failed — most often a QSPI pin mismatch. Check board_config.h.
    Serial.println("[display] Arduino_GFX begin() failed — verify QSPI pins");
  }
  g_gfx->fillScreen(RGB565_BLACK);
  setBrightness(LCD_DEFAULT_BRIGHTNESS);

  lv_init();
  size_t bufPx = (size_t)LCD_WIDTH * BUF_LINES;
  g_buf1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!g_buf1) {
    Serial.println("[display] LVGL draw-buffer alloc failed — reduce BUF_LINES");
    return;  // don't hand LVGL null buffers
  }
  lv_disp_draw_buf_init(&g_drawBuf, g_buf1, nullptr, bufPx);

  lv_disp_drv_init(&g_dispDrv);
  g_dispDrv.hor_res = LCD_WIDTH;
  g_dispDrv.ver_res = LCD_HEIGHT;
  g_dispDrv.flush_cb = flushCb;
  g_dispDrv.draw_buf = &g_drawBuf;
  lv_disp_drv_register(&g_dispDrv);

  lv_indev_drv_init(&g_indevDrv);
  g_indevDrv.type = LV_INDEV_TYPE_POINTER;
  g_indevDrv.read_cb = touchReadCb;
  lv_indev_drv_register(&g_indevDrv);
}

// ---- AMOLED burn-in mitigation ---------------------------------------------
// This is a static instrument UI on an OLED: "VOLTAGE", "CURRENT", the status
// strip and the card outlines sit in exactly the same pixels for hours at a
// time, and on an AMOLED that is permanent damage, not a temporary artefact.
// Two cheap defences, both on by default:
//
//   1. Pixel shift — the whole UI creeps around a 3x3 px box, one step every
//      PIXEL_SHIFT_MS. No edge sits over the same subpixel for more than a few
//      minutes, which is what smears the wear out. Applied as a translate on
//      the active screen and the overlay layer, so it costs one redraw per step
//      and needs no cooperation from the UI code.
//   2. Idle dim, then black — after idleDimS of no touch the panel drops to a
//      readable-but-dark level; after IDLE_SLEEP_FACTOR x that it blanks
//      entirely (an AMOLED drawing black uses no backlight power at all).
//      Either state ends on the first touch. Sleep is suppressed while
//      inhibitSleep(true) is set, so a long unattended test keeps its (dimmed)
//      screen instead of going dark mid-run.
static const uint32_t PIXEL_SHIFT_MS = 90000;
static const int IDLE_SLEEP_FACTOR = 5;
static const uint8_t IDLE_DIM_LEVEL = 24;   // out of 255

static bool g_pixelShift = true;
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
    g_amoled->setBrightness(min(IDLE_DIM_LEVEL, g_brightness));
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
static const uint32_t SMALL_BUF_LINES = 16;   // ~11.5 KB, frees the most for Wi-Fi
static bool g_lowMem = false;

static bool allocDrawBuf(uint32_t lines) {
  size_t bufPx = (size_t)LCD_WIDTH * lines;
  lv_color_t *nb = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t),
                                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!nb) return false;
  g_buf1 = nb;
  lv_disp_draw_buf_init(&g_drawBuf, g_buf1, nullptr, bufPx);
  return true;
}

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
}

bool lowMemMode() { return g_lowMem; }

void loopTick() {
  lv_timer_handler();
  pixelShiftTick();
  idleTick();
}

}  // namespace display
