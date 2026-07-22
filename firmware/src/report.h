// CSV test reports written to the SD card.
//
// One file per test: a commented metadata block (what was run, when, with what
// settings), then the raw samples, then the computed summary. Spreadsheet-
// friendly — the '#' lines import as a single column and the sample block has a
// normal header row, so the curve plots straight from the file.
//
// Header-only, like the test engines it serialises. sd::saveCsv() owns the
// mount/unmount and the file naming; these functions only write the body.
#pragma once

#include <Arduino.h>
#include <stdio.h>

#include "capacity_test.h"
#include "display.h"
#include "resistance_test.h"
#include "sd_card.h"

namespace report {

// Timestamp line: real wall-clock when the RTC has been set, uptime otherwise —
// never a made-up date, so a log's age is always honestly readable.
inline void writeStamp(FILE *f) {
  int Y, M, D, h, m, s;
  if (display::rtcTime(Y, M, D, h, m, s))
    fprintf(f, "# Timestamp,%04d-%02d-%02d %02d:%02d:%02d\n", Y, M, D, h, m, s);
  else
    fprintf(f, "# Timestamp,(RTC not set) uptime %lu s\n",
            (unsigned long)(millis() / 1000));
  fprintf(f, "# Firmware,EL15 Load Control ESP32-C6 (built " __DATE__ ")\n");
}

inline bool saveRTest(const ResistanceTest::Result &r, char *msg, size_t msgLen) {
  return sd::saveCsv("RTEST", [&r](FILE *f) {
    fprintf(f, "# EL15 circuit resistance test\n");
    writeStamp(f);
    fprintf(f, "# Fuse rating (A),%.2f\n", r.fuseRating);
    fprintf(f, "# Max test current (A),%.3f\n", r.maxTestCurrent);
    fprintf(f, "# Probe wiring,%s\n", r.fourWire ? "4-wire (Kelvin)" : "2-wire");
    fprintf(f, "# Lead tare (ohm),%.6f\n", r.tareOhm);
    fprintf(f, "\n# Averaged sample per current level (bidirectional sweep)\n");
    fprintf(f, "level,current_a,voltage_v,temperature_c,fan\n");
    int i = 1;
    for (const ResistanceTest::Sample &s : r.samples)
      fprintf(f, "%d,%.4f,%.4f,%.1f,%d\n", i++, s.current, s.voltage,
              s.temperature, s.fanSpeed);
    fprintf(f, "\n# Result\n");
    fprintf(f, "quantity,value,unit\n");
    fprintf(f, "resistance,%.6f,ohm\n", r.resistanceOhm);
    fprintf(f, "resistance_measured,%.6f,ohm\n", r.rawResistanceOhm);
    fprintf(f, "resistance_std_err,%.6f,ohm\n", r.resistanceStdErr);
    fprintf(f, "open_circuit_voltage,%.4f,V\n", r.openCircuitVoltage);
    fprintf(f, "r_squared,%.5f,\n", r.rSquared);
    fprintf(f, "samples,%d,\n", (int)r.samples.size());
    fprintf(f, "reliable,%s,\n", r.reliable ? "yes" : "no");
    return ferror(f) == 0;
  }, msg, msgLen);
}

inline bool saveBatt(const CapacityTest::Result &r, char *msg, size_t msgLen) {
  return sd::saveCsv("BATT", [&r](FILE *f) {
    fprintf(f, "# EL15 battery capacity test\n");
    writeStamp(f);
    fprintf(f, "# Cutoff voltage (V),%.2f\n", r.cutoffV);
    fprintf(f, "# Discharge current (A),%.3f\n", r.currentA);
    fprintf(f, "\n# Result\n");
    fprintf(f, "quantity,value,unit\n");
    fprintf(f, "capacity,%.4f,Ah\n", r.capacityAh);
    fprintf(f, "energy,%.3f,Wh\n", r.energyWh);
    fprintf(f, "duration,%lu,s\n", (unsigned long)r.durationS);
    fprintf(f, "start_voltage,%.3f,V\n", r.startV);
    fprintf(f, "end_voltage,%.3f,V\n", r.endV);
    fprintf(f, "rebound_voltage,%.3f,V\n", r.reboundV);
    fprintf(f, "average_voltage,%.3f,V\n", r.avgV);
    fprintf(f, "average_current,%.3f,A\n", r.avgI);
    fprintf(f, "min_temperature,%.1f,C\n", r.minTemp);
    fprintf(f, "max_temperature,%.1f,C\n", r.maxTemp);
    fprintf(f, "max_fan,%d,\n", r.maxFan);
    fprintf(f, "stop_reason,%s,\n", r.stopReason);
    // The per-sample discharge curve is not in the Result struct; streaming it
    // live during the test is Phase 3 of CAPACITY_PLAN.md.
    return ferror(f) == 0;
  }, msg, msgLen);
}

}  // namespace report
