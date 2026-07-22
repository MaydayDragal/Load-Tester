#include "sd_card.h"

#include <Arduino.h>
#include <dirent.h>
#include <driver/gpio.h>
#include <driver/sdspi_host.h>
#include <esp_rom_gpio.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <soc/gpio_sig_map.h>
#include <soc/spi_periph.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "board_config.h"

namespace sd {
namespace {

// The panel's Arduino_ESP32QSPI bus lives on SPI2_HOST (its ESP32QSPI_SPI_HOST
// default) and must be constructed with is_shared_interface = true so it
// releases the bus lock between draws — see display.cpp.
constexpr spi_host_device_t HOST = SPI2_HOST;
const char *MOUNT_POINT = "/sd";

sdmmc_card_t *g_card = nullptr;
bool g_pinsReady = false;

// ---- Shared-bus routing ----------------------------------------------------
// The ESP32-C6 has exactly one general-purpose SPI host and the AMOLED already
// owns it, so the card is a second *device* on that same host: same peripheral,
// different pins. The SPI driver handles the per-device parts (clock, mode,
// chip select) but pins are fixed at bus-init time, so we move the clock/data
// signals between the panel's pads and the card's through the GPIO matrix
// around each card access.
//
// This is safe only because both users run on the loop task (the one other task
// in this firmware, audio, is I2S-only): a mount/write/unmount bracket cannot
// interleave with an LVGL flush. NOTHING may draw to the panel between
// routeToCard() and routeToPanel().
void routeToCard() {
  const spi_signal_conn_t &s = spi_periph_signal[HOST];
  // Park the panel's clock and data0 as plain GPIOs first, so card traffic
  // cannot clock the panel (its CS is high anyway — belt and braces).
  esp_rom_gpio_connect_out_signal(LCD_QSPI_SCK, SIG_GPIO_OUT_IDX, false, false);
  esp_rom_gpio_connect_out_signal(LCD_QSPI_D0, SIG_GPIO_OUT_IDX, false, false);
  esp_rom_gpio_connect_out_signal(SD_SPI_SCK, s.spiclk_out, false, false);
  esp_rom_gpio_connect_out_signal(SD_SPI_MOSI, s.spid_out, false, false);
  // Input signals have a single source, so pointing spiq_in at the card's DAT0
  // implicitly takes it off the panel's D1.
  esp_rom_gpio_connect_in_signal(SD_SPI_MISO, s.spiq_in, false);
}

void routeToPanel() {
  const spi_signal_conn_t &s = spi_periph_signal[HOST];
  esp_rom_gpio_connect_out_signal(SD_SPI_SCK, SIG_GPIO_OUT_IDX, false, false);
  esp_rom_gpio_connect_out_signal(SD_SPI_MOSI, SIG_GPIO_OUT_IDX, false, false);
  esp_rom_gpio_connect_out_signal(LCD_QSPI_SCK, s.spiclk_out, false, false);
  esp_rom_gpio_connect_out_signal(LCD_QSPI_D0, s.spid_out, false, false);
  esp_rom_gpio_connect_in_signal(LCD_QSPI_D0, s.spid_in, false);
  esp_rom_gpio_connect_in_signal(LCD_QSPI_D1, s.spiq_in, false);
  // D2/D3 (quad WP/HD) were never re-pointed: the card only ever uses one data
  // line each way, so those signals stay parked on the panel throughout.
}

void preparePins() {
  if (g_pinsReady) return;
  esp_rom_gpio_pad_select_gpio(SD_SPI_SCK);
  gpio_set_direction((gpio_num_t)SD_SPI_SCK, GPIO_MODE_OUTPUT);
  esp_rom_gpio_pad_select_gpio(SD_SPI_MOSI);
  gpio_set_direction((gpio_num_t)SD_SPI_MOSI, GPIO_MODE_OUTPUT);
  esp_rom_gpio_pad_select_gpio(SD_SPI_MISO);
  gpio_set_direction((gpio_num_t)SD_SPI_MISO, GPIO_MODE_INPUT);
  // SPI-mode cards need DAT0 pulled up; the internal pull-up is enough for the
  // short on-board trace if the slot has no external one.
  gpio_set_pull_mode((gpio_num_t)SD_SPI_MISO, GPIO_PULLUP_ONLY);
  g_pinsReady = true;
}

// ---- Mount / unmount -------------------------------------------------------
const char *mountErrorText(esp_err_t e) {
  switch (e) {
    case ESP_FAIL:            return "Card not formatted (use FAT32)";
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_INVALID_RESPONSE:
    case ESP_ERR_INVALID_CRC: return "No card detected";
    case ESP_ERR_NO_MEM:      return "Out of memory";
    default:                  return "Card init failed";
  }
}

// Leaves the bus routed at the card on success; restores the panel on failure.
bool mount(char *msg, size_t msgLen) {
  if (g_card) return true;
  preparePins();
  routeToCard();

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = HOST;
  host.max_freq_khz = SD_SPI_FREQ_KHZ;

  sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
  dev.host_id = HOST;
  dev.gpio_cs = (gpio_num_t)SD_SPI_CS;

  esp_vfs_fat_mount_config_t mcfg = {};
  mcfg.format_if_mount_failed = false;   // never wipe a card the user handed us
  mcfg.max_files = 2;
  mcfg.allocation_unit_size = 16 * 1024;

  esp_err_t e = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev, &mcfg, &g_card);
  if (e != ESP_OK) {
    g_card = nullptr;
    routeToPanel();
    snprintf(msg, msgLen, "%s", mountErrorText(e));
    Serial.printf("[sd] mount failed: %s (0x%x)\n", esp_err_to_name(e), (int)e);
    return false;
  }
  return true;
}

void unmount() {
  if (g_card) {
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, g_card);
    g_card = nullptr;
  }
  routeToPanel();
}

// Next free NNN for `prefix`: one past the highest <prefix>_NNN.* already on the
// card, so a save never overwrites an earlier report.
int nextIndex(const char *prefix) {
  int best = 0;
  size_t plen = strlen(prefix);
  DIR *d = opendir(MOUNT_POINT);
  if (!d) return 1;
  while (dirent *ent = readdir(d)) {
    if (strncasecmp(ent->d_name, prefix, plen) != 0) continue;
    if (ent->d_name[plen] != '_') continue;
    int n = atoi(ent->d_name + plen + 1);
    if (n > best) best = n;
  }
  closedir(d);
  return best + 1;
}

}  // namespace

bool saveCsv(const char *prefix, const std::function<bool(FILE *)> &body,
             char *msg, size_t msgLen) {
  if (!mount(msg, msgLen)) return false;

  int idx = nextIndex(prefix);
  if (idx > 999) {   // rather than silently overwriting report 001
    unmount();
    snprintf(msg, msgLen, "999 %s reports on card - delete some", prefix);
    return false;
  }
  char name[24], path[48];
  snprintf(name, sizeof(name), "%s_%03d.CSV", prefix, idx);
  snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, name);

  FILE *f = fopen(path, "w");
  if (!f) {
    unmount();
    snprintf(msg, msgLen, "Cannot write (card locked or full)");
    return false;
  }
  bool ok = body(f);
  if (ferror(f)) ok = false;
  // fsync before the close so a card yanked straight after "Saved" still has
  // the data: FATFS only pushes the dirty sector + directory entry on sync.
  if (fflush(f) != 0 || fsync(fileno(f)) != 0) ok = false;
  if (fclose(f) != 0) ok = false;
  if (!ok) remove(path);   // no half-written report left claiming to be a result
  unmount();

  if (!ok) {
    snprintf(msg, msgLen, "Write failed - card full?");
    return false;
  }
  Serial.printf("[sd] wrote %s\n", name);
  snprintf(msg, msgLen, "%s", name);
  return true;
}

bool info(char *msg, size_t msgLen) {
  if (!mount(msg, msgLen)) return false;
  uint64_t bytes = (uint64_t)g_card->csd.capacity * g_card->csd.sector_size;
  const char *kind = g_card->is_sdio ? "SDIO" : g_card->is_mmc ? "MMC"
                   : (g_card->ocr & (1 << 30)) ? "SDHC" : "SDSC";
  uint64_t total = 0, freeB = 0;
  bool haveFree = esp_vfs_fat_info(MOUNT_POINT, &total, &freeB) == ESP_OK;
  if (haveFree)
    snprintf(msg, msgLen, "%s %.1f GB (%.1f GB free)", kind, bytes / 1e9,
             freeB / 1e9);
  else
    snprintf(msg, msgLen, "%s %.1f GB", kind, bytes / 1e9);
  unmount();
  return true;
}

}  // namespace sd
