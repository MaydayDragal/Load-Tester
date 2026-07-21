#include "display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <esp_heap_caps.h>

#include "board_config.h"

// SH8601 AMOLED over QSPI via Arduino_GFX. Arduino_GFX ships an Arduino_SH8601
// driver and an Arduino_ESP32QSPI bus; this is the same combination Waveshare's
// demos use for this panel.
static Arduino_DataBus *g_bus = new Arduino_ESP32QSPI(
    LCD_QSPI_CS, LCD_QSPI_SCK, LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);
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
// Partial buffer: 1/8 of the frame in two banks (double-buffered). AMOLED is
// 16bpp; a full framebuffer (368*448*2 = ~322 KB) is too large for on-chip RAM,
// so partial refresh is the right choice here.
static const uint32_t BUF_LINES = LCD_HEIGHT / 8;
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
  return 1;
}

static void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
  // Hold the last reported state on a transient I2C failure: reporting a spurious
  // RELEASE mid-press makes LVGL drop the click and taps feel unresponsive.
  static lv_indev_state_t lastState = LV_INDEV_STATE_REL;
  static lv_point_t lastPt = {0, 0};
  uint16_t x, y;
  int r = readTouch(x, y);
  if (r == 1) { lastState = LV_INDEV_STATE_PR; lastPt.x = x; lastPt.y = y; }
  else if (r == 0) { lastState = LV_INDEV_STATE_REL; }
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
  delay(20);
}

void setBrightness(uint8_t level) {
  // Arduino_SH8601 exposes setBrightness() (SH8601 command 0x51). If your
  // Arduino_GFX version predates it, update the library or send 0x51 via the
  // bus here.
  g_amoled->setBrightness(level);
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

  if (!g_gfx->begin()) {
    // Panel bring-up failed — most often a QSPI pin mismatch. Check board_config.h.
    Serial.println("[display] Arduino_GFX begin() failed — verify QSPI pins");
  }
  g_gfx->fillScreen(RGB565_BLACK);
  setBrightness(LCD_DEFAULT_BRIGHTNESS);

  lv_init();
  size_t bufPx = (size_t)LCD_WIDTH * BUF_LINES;
  g_buf1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  g_buf2 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!g_buf1 || !g_buf2) {
    Serial.println("[display] LVGL draw-buffer alloc failed — reduce BUF_LINES");
    return;  // don't hand LVGL null buffers
  }
  lv_disp_draw_buf_init(&g_drawBuf, g_buf1, g_buf2, bufPx);

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

void loopTick() {
  lv_timer_handler();
}

}  // namespace display
