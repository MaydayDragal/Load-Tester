#include "sd_card.h"

#include <Arduino.h>
// SdFat skips its own `File` typedef whenever <FS.h> is merely REACHABLE on the
// include path, and says so with a #warning. Arduino's FS.h has been on the path
// since sample_log.cpp pulled in LittleFS, so that warning now fires on every
// build of this file. It is purely informational — nothing here uses SdFat's
// `File`; the FAT-only `File32` is named explicitly throughout.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#include <SdFat.h>
#pragma GCC diagnostic pop
#include <string.h>
#include <strings.h>

#include "board_config.h"
#include "display.h"   // rtcTime() — real timestamps on the FAT directory entries

// microSD on BIT-BANGED (software) SPI.
//
// The ESP32-C6 has exactly one general-purpose SPI host and the AMOLED's
// Arduino_ESP32QSPI driver owns it in quad mode. The IDF `sdspi` driver cannot
// run transactions on that bus while the panel holds it — a real card gets as
// far as attaching and then fails at the very first init command (CMD59 CRC
// on/off returns ESP_ERR_NOT_SUPPORTED). That was the old shared-bus scheme,
// and it never worked against hardware.
//
// So the card is driven entirely in software on its own dedicated pins
// (SCK 11 / MOSI 10 / MISO 18 / CS 6), through the small custom SdSpiBaseClass
// driver below (SdFat's own SoftSpiDriver was too fast — see its comment).
// Nothing touches SPI2, so there is no conflict with the panel and no GPIO-matrix
// rerouting — a card access and a screen redraw are fully independent. Software
// SPI tops out around a few hundred kHz, which is plenty for the small CSV
// reports this writes; a save still blocks the loop task for a moment (card
// init dominates), so the UI paints its "Saving..." state before calling in.

namespace sd {
namespace {

// ---- Custom software-SPI driver --------------------------------------------
// SdFat's built-in SoftSpiDriver uses DigitalIO fast-register GPIO, which on the
// 160 MHz C6 clocks fast enough that 512-byte data-block writes corrupt (short
// commands survive, block writes don't). This driver bit-bangs with plain
// Arduino digitalWrite/digitalRead instead — their ~1 us call overhead sets an
// inherently slow, reliable SPI clock (~200-300 kHz, well inside the SD spec),
// and MISO carries an internal pull-up. SPI mode 0, MSB first. SdFat owns the CS
// pin (via SdSpiConfig), so this driver only touches SCK/MOSI/MISO.
class SoftSpi : public SdSpiBaseClass {
 public:
  void begin(SdSpiConfig) override {
    pinMode(SD_SPI_SCK, OUTPUT);  digitalWrite(SD_SPI_SCK, LOW);   // mode 0 idle low
    pinMode(SD_SPI_MOSI, OUTPUT); digitalWrite(SD_SPI_MOSI, HIGH);
    pinMode(SD_SPI_MISO, INPUT_PULLUP);
  }
  void activate() override {}
  void deactivate() override {}
  void end() override {}
  void setSckSpeed(uint32_t) override {}

  uint8_t receive() override { return transfer(0xFF); }
  uint8_t receive(uint8_t *buf, size_t count) override {
    for (size_t i = 0; i < count; i++) buf[i] = transfer(0xFF);
    return 0;   // 0 = success
  }
  void send(uint8_t data) override { transfer(data); }
  void send(const uint8_t *buf, size_t count) override {
    for (size_t i = 0; i < count; i++) transfer(buf[i]);
  }

 private:
  // One byte, full-duplex, SPI mode 0: MOSI set while clock low, both ends
  // sample on the rising edge, shift on the falling edge.
  static inline uint8_t transfer(uint8_t out) {
    uint8_t in = 0;
    for (int i = 0; i < 8; i++) {
      digitalWrite(SD_SPI_MOSI, (out & 0x80) ? HIGH : LOW);
      out <<= 1;
      digitalWrite(SD_SPI_SCK, HIGH);          // rising edge
      in <<= 1;
      if (digitalRead(SD_SPI_MISO)) in |= 1;   // sample MISO
      digitalWrite(SD_SPI_SCK, LOW);           // falling edge
    }
    return in;
  }
};

SoftSpi g_softSpi;
// SHARED_SPI (not DEDICATED): deselect the card between every operation. Over
// this bit-banged link a DEDICATED multi-block write was being left un-terminated
// so the next file open failed; SHARED forces a clean CS cycle per op.
#define SD_CONFIG SdSpiConfig(SD_SPI_CS, SHARED_SPI, SD_SCK_MHZ(0), &g_softSpi)

// FAT16/FAT32 only (File32) — the format essentially every SD card ships in and
// what the reports need; avoids pulling in the larger exFAT code.
SdFat32 g_sd;
bool g_mounted = false;

// ---- FAT directory timestamps ----------------------------------------------
// Without this callback SdFat stamps every file it creates with a fixed default
// date, so a report written today lands on the card as 2000-01-01 and a PC's
// file listing shows the wrong date even though the CSV body and the Settings
// clock are both right. SdFat asks for the time on create, on write and on
// close, so the file carries a real created/modified date once the RTC is set.
// (When the RTC has never been set, rtcTime() fails and we leave the fields at
// 0 — an obviously-absent date rather than an invented one.)
void fatDateTime(uint16_t *date, uint16_t *time) {
  int Y, M, D, h, m, s;
  if (!display::rtcTime(Y, M, D, h, m, s)) { *date = 0; *time = 0; return; }
  *date = FS_DATE(Y, M, D);
  *time = FS_TIME(h, m, s);
}

// ---- Mount / unmount -------------------------------------------------------

// Is the physical card still the one we mounted? CMD10 (read CID) is a real
// round-trip to the card, so it fails both when the card is gone and when it was
// pulled and re-inserted (a re-inserted card is back in native SD mode and
// ignores SPI commands until CMD0/ACMD41 run again). Cheap: a few dozen bytes
// over the bit-banged link.
bool cardResponds() {
  cid_t cid;
  return g_sd.card() && g_sd.card()->readCID(&cid);
}

bool mount(char *msg, size_t msgLen) {
  // Trust an existing mount only while the card still answers. This is what
  // makes eject/re-insert work without a reboot: the stale mount is dropped and
  // the retry loop below runs a full init on the new card.
  if (g_mounted && !cardResponds()) {
    Serial.println("[sd] card stopped answering (removed?) - remounting");
    g_sd.end();
    g_mounted = false;
  }
  if (g_mounted) return true;
  FsDateTime::setCallback(fatDateTime);
  // Init over software SPI is occasionally flaky (a marginal CMD0, or a card
  // still wedged mid-transfer from an earlier aborted write). Retry a few times
  // with the driver fully torn down between attempts before giving up.
  uint8_t ec = 0, ed = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    if (g_sd.begin(SD_CONFIG)) {
      g_mounted = true;
      return true;
    }
    ec = g_sd.sdErrorCode();
    ed = g_sd.sdErrorData();
    Serial.printf("[sd] begin attempt %d failed: errorCode=0x%02X R1=0x%02X\n",
                  attempt + 1, ec, ed);
    g_sd.end();
    delay(20);
  }
  // errorCode != 0 means the card never answered as expected (absent, wedged, or
  // bad wiring); a clean card with no error code means it answered but the volume
  // is not FAT — almost always "not formatted FAT32".
  if (ec) snprintf(msg, msgLen, "No card detected (reseat it)");
  else snprintf(msg, msgLen, "Card not formatted (use FAT32)");
  return false;
}

}  // namespace

void invalidate() {
  if (g_mounted) {
    g_sd.end();
    g_mounted = false;
    Serial.println("[sd] mount invalidated - next access will re-init the card");
  }
}

namespace {

// The card is on dedicated pins that nothing else touches, so — unlike the old
// shared-bus scheme — there is no reason to unmount between operations, and
// re-running the full card-init handshake over software SPI is flaky (an
// end()+begin() cycle intermittently fails CMD0). So mount ONCE and stay
// mounted; this is only here for completeness / an explicit teardown, and is
// only actually called by the EL15_SDTEST scaffolding — hence maybe_unused.
[[maybe_unused]] void unmount() {
  if (g_mounted) {
    g_sd.end();
    g_mounted = false;
  }
}

// Next free NNN for `prefix`: one past the highest <prefix>_NNN.* already on the
// card, so a save never overwrites an earlier report.
int nextIndex(const char *prefix) {
  int best = 0;
  size_t plen = strlen(prefix);
  File32 dir, f;
  if (!dir.open("/")) return 1;
  char name[64];
  while (f.openNext(&dir, O_RDONLY)) {
    f.getName(name, sizeof(name));
    f.close();
    if (strncasecmp(name, prefix, plen) == 0 && name[plen] == '_') {
      int n = atoi(name + plen + 1);
      if (n > best) best = n;
    }
  }
  dir.close();
  return best + 1;
}

}  // namespace

bool saveCsv(const char *prefix, const std::function<bool(Print &)> &body,
             char *msg, size_t msgLen) {
  if (!mount(msg, msgLen)) return false;

  int idx = nextIndex(prefix);
  if (idx > 999) {   // rather than silently overwriting report 001
    snprintf(msg, msgLen, "999 %s reports on card - delete some", prefix);
    return false;
  }
  char name[24];
  snprintf(name, sizeof(name), "%s_%03d.CSV", prefix, idx);

  File32 f;
  if (!f.open(name, O_WRITE | O_CREAT | O_TRUNC)) {
    // Could be a locked/full card — or one that was pulled since the last save
    // and is still serving a cached FAT. Drop the mount either way so the next
    // attempt re-inits rather than failing forever against a dead handle.
    invalidate();
    snprintf(msg, msgLen, "Cannot write (card removed, locked or full)");
    return false;
  }
  bool ok = body(f);
  if (f.getWriteError()) ok = false;
  // sync() is where the LAST cache block and the directory entry actually reach
  // the card, and close() flushes again — so they are the most likely place for a
  // late failure, and both were being called for their side effect with their
  // result thrown away. That made "the tail of the file never landed" a silent
  // outcome, which is exactly what this module exists not to do: a report is
  // either written or honestly reported as failed.
  if (!f.sync()) ok = false;
  if (f.getWriteError()) ok = false;
  if (!f.close()) ok = false;
  if (!ok) g_sd.remove(name);   // no half-written report left claiming to be a result

  if (!ok) {
    invalidate();   // a mid-write failure usually means the card went away
    snprintf(msg, msgLen, "Write failed - card removed or full?");
    return false;
  }
  Serial.printf("[sd] wrote %s\n", name);
  snprintf(msg, msgLen, "%s", name);
  return true;
}

bool info(char *msg, size_t msgLen) {
  // "Check card" is the user's way of saying "I just changed the card" — always
  // re-init rather than reporting whatever was mounted before.
  invalidate();
  if (!mount(msg, msgLen)) return false;
  uint64_t bytes = (uint64_t)g_sd.card()->sectorCount() * 512ull;
  uint8_t ct = g_sd.card()->type();
  const char *kind = ct == SD_CARD_TYPE_SDHC ? "SDHC"
                   : ct == SD_CARD_TYPE_SD2  ? "SDSC"
                   : ct == SD_CARD_TYPE_SD1  ? "SD"
                                             : "SD";
  // Report type + size only. Free space needs freeClusterCount(), which walks
  // the whole FAT — many seconds over software SPI on a 32 GB card, and it was
  // returning an error sentinel anyway. Size is what the user actually wants.
  snprintf(msg, msgLen, "%s %.1f GB", kind, bytes / 1e9);
  return true;
}

#ifdef EL15_SDTEST
// Test-only: read a file straight back so a self-test can prove the bytes it
// wrote actually landed on the card.
bool readBackTest(const char *name, char *msg, size_t msgLen) {
  if (!mount(msg, msgLen)) return false;
  File32 f;
  if (!f.open(name, O_RDONLY)) {
    unmount();
    snprintf(msg, msgLen, "reopen failed");
    return false;
  }
  // The FAT directory timestamp is what a PC's file listing shows, and it comes
  // from the fatDateTime() callback, NOT from anything in the file body — so
  // print it separately to prove the callback is wired up.
  uint16_t fdate = 0, ftime = 0;
  if (f.getModifyDateTime(&fdate, &ftime)) {
    Serial.printf("[sdtest]   FAT modify stamp: %04d-%02d-%02d %02d:%02d:%02d\n",
                  1980 + (fdate >> 9), (fdate >> 5) & 0x0F, fdate & 0x1F,
                  ftime >> 11, (ftime >> 5) & 0x3F, (ftime & 0x1F) * 2);
  } else {
    Serial.println("[sdtest]   FAT modify stamp: none");
  }
  int lines = 0;
  while (f.available() && lines < 8) {
    char line[80];
    int n = f.fgets(line, sizeof(line));
    if (n <= 0) break;
    Serial.printf("[sdtest]   %s", line);
    lines++;
  }
  f.close();
  snprintf(msg, msgLen, "%d lines", lines);
  return true;
}
#endif

}  // namespace sd
