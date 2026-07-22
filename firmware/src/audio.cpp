#include "audio.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "board_config.h"

extern "C" {
#include "es8311.h"
}

namespace audio {

static const int SAMPLE_RATE = 16000;

static I2SClass g_i2s;
static bool g_ready = false;
static uint8_t g_volume = 70;   // 0..100
static bool g_muted = false;

static QueueHandle_t g_queue = nullptr;

// One note. freq 0 = a silent gap. amp is 0..100 before volume/mute scaling.
struct Note { uint16_t freq; uint16_t durMs; uint8_t amp; };

// Drive the speaker-amp enable bit on the shared TCA9554 expander high, without
// disturbing the panel bits already set there (read-modify-write, same pattern
// as display.cpp's panel enable).
static void ampEnable() {
  auto rmw = [](uint8_t reg, uint8_t setMask, uint8_t clrMask) {
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
  rmw(0x03, 0x00, SPK_AMP_EN_BIT);  // config: 0 = output
  rmw(0x01, SPK_AMP_EN_BIT, 0x00);  // output: drive high (amp on)
}

static bool es8311Init() {
  es8311_handle_t h = es8311_create(ES8311_I2C_PORT, ES8311_I2C_ADDR);
  if (!h) return false;
  es8311_clock_config_t clk = {};
  clk.mclk_inverted = false;
  clk.sclk_inverted = false;
  clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency = SAMPLE_RATE * 256;
  clk.sample_frequency = SAMPLE_RATE;
  if (es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  es8311_sample_frequency_config(h, clk.mclk_frequency, clk.sample_frequency);
  es8311_microphone_config(h, false);        // playback only
  es8311_voice_volume_set(h, 80, nullptr);   // codec headroom; loudness via PCM
  return true;
}

// Continuous-stream audio task. The I2S is written back-to-back the whole time
// sound is active — the current tone, then queued tones, then silence — never
// stopping and restarting the stream between tones. Stopping/restarting the
// write stream per tone was audible as a glitch (short tones "cut in and out")
// while one long continuous write was clean; this makes every tone behave like
// that clean continuous write. After KEEPALIVE_MS of silence the task goes idle
// (blocks on the queue) so it isn't streaming forever.
static const int STREAM_FRAMES = 128;        // ~8 ms per write; low latency
static const uint32_t KEEPALIVE_MS = 2500;   // keep the stream up between bursts

static void audioTask(void *) {
  static int16_t buf[STREAM_FRAMES * 2];
  Note cur{};
  bool haveCur = false;
  int pos = 0, total = 0, atk = 0, rel = 0;
  float phase = 0, dph = 0, amp = 0;
  bool streaming = false;
  uint32_t lastSoundMs = 0;

  for (;;) {
    // Get a note to render if we have none. Block when idle; poll when streaming
    // so a chime's next note is picked up seamlessly within the same stream.
    if (!haveCur) {
      Note n;
      if (xQueueReceive(g_queue, &n, streaming ? 0 : portMAX_DELAY) == pdTRUE) {
        cur = n;
        total = (int)((uint32_t)n.durMs * SAMPLE_RATE / 1000);
        pos = 0; phase = 0;
        dph = 2.0f * (float)M_PI * n.freq / SAMPLE_RATE;
        atk = min(total / 4, SAMPLE_RATE * 4 / 1000);
        rel = min(total / 4, SAMPLE_RATE * 6 / 1000);
        amp = (!g_muted && g_volume > 0 && n.amp > 0 && n.freq > 0)
                  ? (n.amp / 100.0f) * (g_volume / 100.0f) * 0.5f * 32767.0f
                  : 0.0f;   // freq 0 / muted -> in-stream silence of durMs
        haveCur = total > 0;
        streaming = true;
        lastSoundMs = millis();
      }
    }

    for (int k = 0; k < STREAM_FRAMES; k++) {
      int16_t s = 0;
      if (haveCur && pos < total) {
        float env = 1.0f;
        if (pos < atk) env = (float)pos / atk;
        else if (pos > total - rel) env = (float)(total - pos) / rel;
        s = (int16_t)(amp * env * sinf(phase));
        phase += dph;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        if (++pos >= total) { haveCur = false; lastSoundMs = millis(); }
      }
      buf[k * 2] = s;
      buf[k * 2 + 1] = s;
    }

    if (streaming) {
      g_i2s.write((uint8_t *)buf, sizeof(buf));   // paced by the DMA
      if (!haveCur && (millis() - lastSoundMs) > KEEPALIVE_MS) streaming = false;
    }
  }
}

void tone(uint16_t freqHz, uint16_t durMs, uint8_t ampPct) {
  if (!g_ready || !g_queue) return;
  Note n{freqHz, durMs, ampPct};
  xQueueSend(g_queue, &n, 0);   // drop if the queue is backed up
}

void begin() {
  g_i2s.setPins(I2S_BCLK_GPIO, I2S_WS_GPIO, I2S_DOUT_GPIO, I2S_DIN_GPIO, I2S_MCLK_GPIO);
  if (!g_i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[audio] I2S begin failed — sound disabled");
    return;
  }
  ampEnable();
  if (!es8311Init()) {
    Serial.println("[audio] ES8311 init failed — sound disabled");
    return;
  }
  g_queue = xQueueCreate(16, sizeof(Note));
  if (!g_queue) return;
  // Priority 8 — above the Arduino loop (1) so the task can preempt a long QSPI
  // redraw to refill the I2S DMA (otherwise tones drop out / sound choppy when a
  // tap fires a click and a screen redraw at the same time). It only runs in
  // brief refill bursts (it blocks on the queue when idle and on DMA space while
  // playing), so it doesn't starve the UI or BLE.
  xTaskCreate(audioTask, "audio", 4096, nullptr, 8, nullptr);
  g_ready = true;
  Serial.println("[audio] ready (ES8311)");
}

bool ready() { return g_ready; }
void setVolume(uint8_t pct) { g_volume = pct > 100 ? 100 : pct; }
uint8_t getVolume() { return g_volume; }
void setMuted(bool m) { g_muted = m; }
bool muted() { return g_muted; }

// ---- Semantic feedback -----------------------------------------------------
void click()   { tone(2600, 9, 22); }
void press()   { tone(1900, 28, 45); }
void success() { tone(880, 90, 55); tone(0, 25, 0); tone(1320, 150, 55); }
void failure() { tone(660, 110, 55); tone(0, 25, 0); tone(440, 190, 55); }
void fault()   { tone(1760, 110, 85); tone(0, 60, 0); tone(1245, 110, 85);
                 tone(0, 60, 0); tone(1760, 140, 85); }

}  // namespace audio
