// CSV test reports written to the SD card.
//
// One file per test: a commented metadata block (what was run, when, with what
// settings), then the raw samples, then the computed summary. Spreadsheet-
// friendly — the '#' lines import as a single column and the sample block has a
// normal header row, so the curve plots straight from the file.
//
// Header-only, like the test engines it serialises. sd::saveCsv() owns the
// mount/unmount and the file naming; these functions only write the body, to the
// Print interface of the open file (SdFat runs the card on software SPI now).
#pragma once

#include <Arduino.h>
#include <stdarg.h>

#include "capacity_test.h"
#include "display.h"
#include "resistance_test.h"
#include "sample_log.h"
#include "sd_card.h"

namespace report {

// printf into an open file. SdFat's File is a Print (print()/println()) but has
// no printf, so format into a stack buffer and write it. 192 bytes covers every
// line these reports emit.
inline void fpf(Print &p, const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
  p.write((const uint8_t *)buf, (size_t)n);
}

// Timestamp lines: real wall-clock when the RTC has been set, uptime otherwise —
// never a made-up date, so a log's age is always honestly readable.
//
// Date and time go out as separate fields as well as one combined string: a
// spreadsheet parses "2026-08-01" and "14:03:22" natively, whereas the combined
// form is locale-dependent and often imports as text. The card's own FAT
// directory entry is stamped from the same RTC (see sd_card.cpp fatDateTime),
// so the file's modified date in a file browser matches what is written here.
inline void writeStamp(Print &f) {
  int Y, M, D, h, m, s;
  if (display::rtcTime(Y, M, D, h, m, s)) {
    fpf(f, "# Date,%04d-%02d-%02d\n", Y, M, D);
    fpf(f, "# Time,%02d:%02d:%02d\n", h, m, s);
    fpf(f, "# Timestamp,%04d-%02d-%02d %02d:%02d:%02d\n", Y, M, D, h, m, s);
  } else {
    fpf(f, "# Date,(RTC not set)\n");
    fpf(f, "# Time,(RTC not set)\n");
    fpf(f, "# Timestamp,(RTC not set) uptime %lu s\n",
        (unsigned long)(millis() / 1000));
  }
  fpf(f, "# Firmware,EL15 Load Control ESP32-C6 (built " __DATE__ ")\n");
}

inline bool saveRTest(const ResistanceTest::Result &r, char *msg, size_t msgLen) {
  return sd::saveCsv("RTEST", [&r](Print &f) {
    fpf(f, "# EL15 circuit resistance test\n");
    writeStamp(f);
    fpf(f, "# Fuse rating (A),%.2f\n", r.fuseRating);
    fpf(f, "# Max test current (A),%.3f\n", r.maxTestCurrent);
    fpf(f, "# Probe wiring,%s\n", r.fourWire ? "4-wire (Kelvin)" : "2-wire");
    fpf(f, "# Lead tare (ohm),%.6f\n", r.tareOhm);
    fpf(f, "\n# Averaged sample per current level (bidirectional sweep)\n");
    fpf(f, "level,current_a,voltage_v,temperature_c,fan\n");
    int i = 1;
    for (const ResistanceTest::Sample &s : r.samples)
      fpf(f, "%d,%.4f,%.4f,%.1f,%d\n", i++, s.current, s.voltage,
          s.temperature, s.fanSpeed);
    fpf(f, "\n# Result\n");
    fpf(f, "quantity,value,unit\n");
    fpf(f, "resistance,%.6f,ohm\n", r.resistanceOhm);
    fpf(f, "resistance_measured,%.6f,ohm\n", r.rawResistanceOhm);
    fpf(f, "resistance_std_err,%.6f,ohm\n", r.resistanceStdErr);
    fpf(f, "open_circuit_voltage,%.4f,V\n", r.openCircuitVoltage);
    fpf(f, "r_squared,%.5f,\n", r.rSquared);
    fpf(f, "samples,%d,\n", (int)r.samples.size());
    fpf(f, "reliable,%s,\n", r.reliable ? "yes" : "no");
    return f.getWriteError() == 0;
  }, msg, msgLen);
}

inline bool saveBatt(const CapacityTest::Result &r, char *msg, size_t msgLen) {
  return sd::saveCsv("BATT", [&r](Print &f) {
    fpf(f, "# EL15 battery capacity test\n");
    writeStamp(f);
    fpf(f, "# Cutoff voltage (V),%.2f\n", r.cutoffV);
    fpf(f, "# Discharge current (A),%.3f\n", r.currentA);
    if (r.ratedAh > 0) {
      fpf(f, "# Rated capacity (Ah),%.3f\n", r.ratedAh);
      fpf(f, "# Rated capacity (mAh),%.0f\n", r.ratedAh * 1000.0f);
      fpf(f, "# Discharge rate (C),%.3f\n", r.cRate);
    }
    fpf(f, "\n# Result\n");
    fpf(f, "quantity,value,unit\n");
    fpf(f, "capacity,%.4f,Ah\n", r.capacityAh);
    fpf(f, "capacity_mah,%.1f,mAh\n", r.capacityAh * 1000.0f);
    fpf(f, "energy,%.3f,Wh\n", r.energyWh);
    fpf(f, "duration,%lu,s\n", (unsigned long)r.durationS);
    fpf(f, "paused,%lu,s\n", (unsigned long)r.pausedS);
    fpf(f, "start_voltage,%.3f,V\n", r.startV);
    fpf(f, "end_voltage,%.3f,V\n", r.endV);
    fpf(f, "rebound_voltage,%.3f,V\n", r.reboundV);
    fpf(f, "average_voltage,%.3f,V\n", r.avgV);
    fpf(f, "average_current,%.3f,A\n", r.avgI);
    fpf(f, "average_power,%.3f,W\n", r.avgV * r.avgI);
    fpf(f, "min_temperature,%.1f,C\n", r.minTemp);
    fpf(f, "max_temperature,%.1f,C\n", r.maxTemp);
    fpf(f, "max_fan,%d,\n", r.maxFan);
    fpf(f, "stop_reason,%s,\n", r.stopReason);
    if (r.ratedAh > 0) {
      fpf(f, "rated_capacity,%.4f,Ah\n", r.ratedAh);
      fpf(f, "state_of_health,%.1f,%%\n", r.sohPct);
      fpf(f, "discharge_rate,%.3f,C\n", r.cRate);
    }

    // ---- Per-sample datapoints ----------------------------------------------
    // Streamed straight out of the flash log written during the run, one CSV row
    // at a time — nothing large is ever assembled in RAM. Rows carry their own
    // elapsed time because the log coarsens its sample interval on long runs
    // (see sample_log.h), so the spacing is not necessarily uniform.
    uint32_t n = samplelog::count();
    fpf(f, "\n# Datapoints (%lu samples, %lu s resolution at the end of the run)\n",
        (unsigned long)n, (unsigned long)samplelog::intervalS());
    fpf(f, "elapsed_s,voltage_v,current_a,power_w,capacity_ah,capacity_mah,energy_wh,temperature_c\n");
    if (n > 0) {
      samplelog::replay([&f](const samplelog::Rec &s) {
        fpf(f, "%lu,%.4f,%.4f,%.4f,%.5f,%.2f,%.4f,%.1f\n",
            (unsigned long)s.tS, s.v, s.i, s.v * s.i, s.ah, s.ah * 1000.0f,
            s.wh, s.temp);
        return f.getWriteError() == 0;   // stop early if the card gave up
      });
    }
    return f.getWriteError() == 0;
  }, msg, msgLen);
}

}  // namespace report
