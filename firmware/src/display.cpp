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
static Arduino_SH8601 *g_amoled = new Arduino_SH8601(
    g_bus, LCD_RST_GPIO, 0 /* rotation */, false /* IPS */, LCD_WIDTH, LCD_HEIGHT);
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
static bool readTouch(uint16_t &x, uint16_t &y) {
  // FT3168/FT6x36-family register map: 0x02 = touch count, 0x03.. = point data.
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(TOUCH_I2C_ADDR, 5);
  if (Wire.available() < 5) return false;
  uint8_t touches = Wire.read() & 0x0F;
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if (touches == 0) return false;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return true;
}

static void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
  uint16_t x, y;
  if (readTouch(x, y)) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

void setBrightness(uint8_t level) {
  // Arduino_SH8601 exposes setBrightness() (SH8601 command 0x51). If your
  // Arduino_GFX version predates it, update the library or send 0x51 via the
  // bus here.
  g_amoled->setBrightness(level);
}

void begin() {
  if (!g_gfx->begin()) {
    // Panel bring-up failed — most often a QSPI pin mismatch. Check board_config.h.
    Serial.println("[display] Arduino_GFX begin() failed — verify QSPI pins");
  }
  g_gfx->fillScreen(BLACK);
  setBrightness(LCD_DEFAULT_BRIGHTNESS);

  Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
  Wire.setClock(400000);

  lv_init();
  size_t bufPx = (size_t)LCD_WIDTH * BUF_LINES;
  g_buf1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  g_buf2 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
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
