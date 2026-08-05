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
// ---- Write verification ----------------------------------------------------
// BATT_007.CSV (2026-08-03) came off the card with two 16 KB regions replaced by
// random bytes. Every byte had left the controller intact and the card had
// ACKNOWLEDGED all of them — USE_SD_CRC would have rejected a corrupted transfer
// and surfaced a write error, which would have deleted the file instead of
// saving it. The card simply did not retain ~22 KB of what it accepted, and
// nothing here could tell.
//
// So a report is no longer trusted because the writes returned success: it is
// checksummed on the way out, read back off the card, and checksummed again.
// That is the only check that can catch a card which lies, and this module's
// whole contract is that a failed save is never reported as a save.
//
// Cost: the read-back roughly doubles save time (a 300 KB capacity report goes
// from ~11 s to ~20 s of blocked loop task). That is a deliberate trade — the
// alternative is silently losing 7 % of a four-hour discharge, which is what
// happened.
const size_t VCHUNK = 8192;      // verification granularity
// 8 KB x 256 = 2 MB of coverage, for 1 KB of RAM. It was 64 chunks (512 KB),
// which sounded generous and was not: BATT_013 — a 8.9 h discharge at 2 s
// logging — came to ~436 KB, inside 20 % of the ceiling. A longer run at a
// finer sample rate goes straight past it, and past it the old code only
// PRINTED a warning and still reported the save as verified.
const int VMAX_CHUNKS = 256;

// CRC-32, reflected, polynomial 0xEDB88320 — the same one zip/PNG use, so a
// value logged here can be checked against any desktop tool. Bitwise rather than
// table-driven: 8 shifts per byte over 300 KB is ~0.15 s, invisible next to the
// ~10 s the card itself takes, and it costs no flash for a table.
uint32_t crc32(uint32_t crc, const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return crc;
}

// A Print that forwards to the file while checksumming what went through it, in
// fixed-size chunks. Chunked rather than one whole-file CRC so a mismatch says
// WHERE the card lost data — the 2026-08-03 fault would have printed "chunk 14
// and chunk 38 differ", which took a byte-level analysis to establish by hand.
// The chunk table lives in .bss, NOT inside the object. saveCsv() builds a
// VerifyingPrint on the Arduino loop task's stack, and that stack already hosts
// FATFS's 255-char long-filename buffer and work area — the reason it is raised
// to 12 KB in platformio.ini. Putting a 1 KB table there too is exactly the kind
// of quiet overflow that would present as a random crash mid-save. saveCsv is
// loop-task-only and non-reentrant, so one shared table is safe.
static uint32_t g_chunkCrc[VMAX_CHUNKS];

class VerifyingPrint : public Print {
 public:
  explicit VerifyingPrint(Print &sink) : sink_(sink) {
    for (int i = 0; i < VMAX_CHUNKS; i++) g_chunkCrc[i] = 0xFFFFFFFFu;
  }
  using Print::write;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buf, size_t len) override {
    size_t w = sink_.write(buf, len);
    accumulate(buf, w);
    // A SHORT write is silent data loss that verification is structurally blind
    // to: the checksum is taken over what the file ACCEPTED, and total_ counts
    // the same bytes, so the read-back describes the truncated file perfectly
    // and matches. The rows that never made it are simply not part of what we
    // claim to have written. Callers cannot catch it either — report.h's fpf()
    // discards the return of every write. So it has to be caught here, and it
    // has to be an error rather than a count.
    if (w != len) {
      Serial.printf("[sd] SHORT WRITE: %u of %u bytes accepted at offset %lu\n",
                    (unsigned)w, (unsigned)len, (unsigned long)total_);
      setWriteError(1);
    }
    // report.h aborts its row loop on getWriteError(), and it is handed THIS
    // object rather than the file — so the file's error state has to surface here
    // or a mid-write failure would go unnoticed.
    if (sink_.getWriteError()) setWriteError(sink_.getWriteError());
    return w;
  }
  void flush() override { sink_.flush(); }

  uint32_t total() const { return total_; }
  bool overflowed() const { return overflow_; }
  uint32_t chunk(int i) const { return g_chunkCrc[i]; }

 private:
  void accumulate(const uint8_t *b, size_t n) {
    while (n) {
      size_t idx = total_ / VCHUNK;
      size_t room = VCHUNK - (total_ % VCHUNK);
      size_t take = n < room ? n : room;
      if (idx < (size_t)VMAX_CHUNKS) g_chunkCrc[idx] = crc32(g_chunkCrc[idx], b, take);
      else overflow_ = true;
      total_ += take;
      b += take;
      n -= take;
    }
  }
  Print &sink_;
  uint32_t total_ = 0;
  bool overflow_ = false;
};

// Read the file back off the card and compare it, chunk by chunk, with what we
// believe we wrote. Logs the byte range of every chunk that differs.
bool verifyOnCard(const char *name, const VerifyingPrint &vp) {
  // Drop SdFat's block cache FIRST. This is what makes the read-back mean
  // anything: without it, the bytes we just wrote can still be sitting in the
  // driver's own RAM, and re-reading them compares our data against a copy of
  // our data. The whole failure mode this guards — a card that acknowledges
  // writes it has not committed to NAND — is invisible to a cached read, which
  // is how BATT_007 and BATT_013 both passed a "verified" save and came back
  // corrupt afterwards. Every byte compared below now has to come off the card.
  g_sd.cacheClear();
  File32 f;
  if (!f.open(name, O_RDONLY)) {
    Serial.printf("[sd] verify: cannot reopen %s\n", name);
    return false;
  }
  uint32_t onCard = f.fileSize();
  if (onCard != vp.total()) {
    Serial.printf("[sd] verify FAILED: %s is %lu B on the card, wrote %lu B\n",
                  name, (unsigned long)onCard, (unsigned long)vp.total());
    f.close();
    return false;
  }

  uint8_t buf[512];
  uint32_t crc = 0xFFFFFFFFu, pos = 0;
  int bad = 0;
  bool ok = true;
  while (pos < onCard) {
    size_t room = VCHUNK - (pos % VCHUNK);
    size_t want = room < sizeof(buf) ? room : sizeof(buf);
    if (want > onCard - pos) want = onCard - pos;
    int got = f.read(buf, want);
    if (got <= 0) {
      Serial.printf("[sd] verify FAILED: read error at byte %lu of %s\n",
                    (unsigned long)pos, name);
      ok = false;
      break;
    }
    crc = crc32(crc, buf, (size_t)got);
    pos += (uint32_t)got;
    // End of a chunk (or of the file): settle up.
    if (pos % VCHUNK == 0 || pos == onCard) {
      int idx = (int)((pos - 1) / VCHUNK);
      if (idx < VMAX_CHUNKS && crc != vp.chunk(idx)) {
        Serial.printf("[sd] verify MISMATCH: bytes %lu..%lu of %s\n",
                      (unsigned long)((uint32_t)idx * VCHUNK),
                      (unsigned long)(pos - 1), name);
        ok = false;
        if (++bad >= 8) {   // enough to characterise it; do not flood the log
          Serial.println("[sd] verify: further mismatches suppressed");
          break;
        }
      }
      crc = 0xFFFFFFFFu;
    }
    // The read takes seconds over the bit-banged link. Yield so the idle task
    // keeps its watchdog fed — the same reason samplelog::replay() does.
    if ((pos & 0x1FFF) == 0) delay(1);
  }
  f.close();
  if (ok) Serial.printf("[sd] verify OK: %s, %lu B read back and matched\n",
                        name, (unsigned long)onCard);
  return ok;
}

// Delete a file we have REJECTED, and say so if the card will not let us. This
// is the last line of the contract: a report that failed verification must not
// be left behind looking like a result. remove()'s return used to be discarded,
// so on a card that was already misbehaving the rejected file could quietly
// survive — which is indistinguishable, from the outside, from a good save.
bool fsRemoveChecked(const char *name) {
  if (!g_sd.exists(name)) return true;
  if (g_sd.remove(name)) return true;
  Serial.printf("[sd] WARNING: could not delete rejected file %s - it is STILL ON THE CARD "
                "and its contents are NOT trustworthy\n", name);
  return false;
}

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

  int base = nextIndex(prefix);
  if (base > 999) {   // rather than silently overwriting report 001
    snprintf(msg, msgLen, "999 %s reports on card - delete some", prefix);
    return false;
  }

  // Files written but rejected by verification. Kept ON the card until an attempt
  // succeeds, deliberately: a retry then allocates FRESH clusters instead of the
  // ones the card just failed to retain, which is the difference between working
  // around a bad region and writing into it a second time. They are deleted
  // before returning either way — a report that failed verification must never be
  // left behind looking like a result.
  const int MAX_ATTEMPTS = 2;
  char suspect[MAX_ATTEMPTS][24];
  int nSuspect = 0;
  bool sweepFailed = false;
  auto sweep = [&]() {
    for (int k = 0; k < nSuspect; k++)
      if (!fsRemoveChecked(suspect[k])) sweepFailed = true;
  };

  for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
    int idx = base + attempt;
    if (idx > 999) break;
    char name[24];
    snprintf(name, sizeof(name), "%s_%03d.CSV", prefix, idx);

    File32 f;
    if (!f.open(name, O_WRITE | O_CREAT | O_TRUNC)) {
      // Could be a locked/full card — or one that was pulled since the last save
      // and is still serving a cached FAT. Drop the mount either way so the next
      // attempt re-inits rather than failing forever against a dead handle.
      sweep();
      invalidate();
      snprintf(msg, msgLen, "Cannot write (card removed, locked or full)");
      return false;
    }

    // Everything the body writes goes through the checksummer on its way to the
    // file, so verification costs no second pass over the source data.
    VerifyingPrint vp(f);
    bool ok = body(vp);
    if (vp.getWriteError() || f.getWriteError()) ok = false;
    // sync() is where the LAST cache block and the directory entry actually reach
    // the card, and close() flushes again — so they are the likeliest place for a
    // late failure, and both used to be called for their side effect with their
    // result thrown away.
    if (!f.sync()) ok = false;
    if (f.getWriteError()) ok = false;
    if (!f.close()) ok = false;

    if (!ok) {
      fsRemoveChecked(name);
      sweep();
      invalidate();   // a mid-write failure usually means the card went away
      snprintf(msg, msgLen, "Write failed - card removed or full?");
      return false;
    }
    if (vp.overflowed()) {
      // Past the checksummed ceiling only part of the file can be compared, and
      // a partly-checked file reported as verified is exactly the false comfort
      // this whole path exists to remove. Fail instead — loudly, and with the
      // number, so the ceiling can be raised deliberately rather than guessed.
      Serial.printf("[sd] report is %lu B, past the %d KB verifiable ceiling - REFUSING to "
                    "call it verified\n",
                    (unsigned long)vp.total(), (int)(VCHUNK * VMAX_CHUNKS / 1024));
      fsRemoveChecked(name);
      sweep();
      snprintf(msg, msgLen, "Report too large to verify - raise VMAX_CHUNKS");
      return false;
    }

    if (verifyOnCard(name, vp)) {
      sweep();
      Serial.printf("[sd] wrote %s (%lu B, verified)\n", name, (unsigned long)vp.total());
      snprintf(msg, msgLen, "%s", name);
      return true;
    }

    snprintf(suspect[nSuspect++], sizeof(suspect[0]), "%s", name);
    Serial.printf("[sd] %s did NOT read back correctly - the card accepted writes "
                  "it did not keep%s\n",
                  name, attempt + 1 < MAX_ATTEMPTS ? "; retrying on fresh clusters" : "");
  }

  sweep();
  invalidate();
  // Name the leftover explicitly when the card would not even let us delete the
  // bad files: "replace the card" and "replace the card AND ignore the files it
  // is still showing you" are different instructions.
  if (sweepFailed)
    snprintf(msg, msgLen, "Card lost data twice AND kept the bad files - replace it");
  else
    snprintf(msg, msgLen, "Card lost data twice - replace the card");
  return false;
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
