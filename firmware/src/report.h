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
    fpf(f, "# Sweep start current (A),%.3f\n", r.startCurrent);
    fpf(f, "# Sweep peak current (A),%.3f\n", r.maxTestCurrent);
    fpf(f, "# Sweep duration (s),%lu\n", (unsigned long)r.sweepSeconds);
    fpf(f, "# Sweep shape,continuous triangular ramp (up then down)\n");
    fpf(f, "# Probe wiring,%s\n", r.fourWire ? "4-wire (Kelvin)" : "2-wire");
    fpf(f, "# Lead tare (ohm),%.6f\n", r.tareOhm);
    // The fit runs on every raw sample; these rows are those samples averaged
    // into current bands, which is what makes a readable V-I curve. Each band
    // holds both the up-ramp and down-ramp visit to that current, so the average
    // is where the time-drift cancellation lands.
    fpf(f, "\n# Averaged sample per current band (both ramp directions)\n");
    fpf(f, "band,current_a,voltage_v,power_w,temperature_c,fan\n");
    int i = 1;
    for (const ResistanceTest::Sample &s : r.samples)
      fpf(f, "%d,%.4f,%.4f,%.3f,%.1f,%d\n", i++, s.current, s.voltage,
          s.voltage * s.current, s.temperature, s.fanSpeed);
    fpf(f, "\n# Result\n");
    fpf(f, "quantity,value,unit\n");
    fpf(f, "resistance,%.6f,ohm\n", r.resistanceOhm);
    fpf(f, "resistance_measured,%.6f,ohm\n", r.rawResistanceOhm);
    fpf(f, "resistance_std_err,%.6f,ohm\n", r.resistanceStdErr);
    fpf(f, "open_circuit_voltage,%.4f,V\n", r.openCircuitVoltage);
    fpf(f, "r_squared,%.5f,\n", r.rSquared);
    fpf(f, "raw_samples,%d,\n", r.rawSamples);
    fpf(f, "current_bands,%d,\n", (int)r.samples.size());
    fpf(f, "current_min,%.4f,A\n", r.minCurrent);
    fpf(f, "current_max,%.4f,A\n", r.maxCurrentSeen);
    fpf(f, "voltage_sag,%.4f,V\n", r.sagV);
    fpf(f, "peak_power,%.3f,W\n", r.peakPowerW);
    fpf(f, "temperature_min,%.1f,C\n", r.tempMin);
    fpf(f, "temperature_max,%.1f,C\n", r.tempMax);
    fpf(f, "max_fan,%d,\n", r.maxFan);
    fpf(f, "load_dropouts,%d,\n", r.loadDropouts);
    // Samples the fit deliberately left out, so raw_samples above is auditable
    // against the datapoint block below, which keeps every packet.
    fpf(f, "excluded_step_transient,%d,\n", r.excludedTransient);
    fpf(f, "excluded_repeat_frame,%d,\n", r.excludedDuplicate);
    fpf(f, "excluded_off_target,%d,\n", r.excludedOffTarget);
    fpf(f, "reliable,%s,\n", r.reliable ? "yes" : "no");

    // ---- Per-sample datapoints ----------------------------------------------
    // Streamed straight out of the flash log written during the sweep, exactly
    // like the capacity report's block — at the rtest log's 25 ms base interval
    // that is every status packet for any ordinary sweep. Includes the samples
    // the fit EXCLUDED (load-off dropouts, settle transients): the banded curve
    // above is what was fitted, this block is what happened. target_a is the
    // commanded setpoint at that instant, so current_a vs target_a shows the
    // load's regulation lag; a dropout reads as current_a ~ 0 under a nonzero
    // target.
    uint32_t n = samplelog::rtest.count();
    fpf(f, "\n# Datapoints (%lu samples, %lu ms resolution at the end of the sweep)\n",
        (unsigned long)n, (unsigned long)samplelog::rtest.intervalMs());
    fpf(f, "elapsed_s,target_a,voltage_v,current_a,power_w,temperature_c,fan\n");
    if (n > 0) {
      bool logOk = samplelog::rtest.replay([&f](const samplelog::Rec &s) {
        fpf(f, "%.3f,%.3f,%.4f,%.4f,%.3f,%.1f,%d\n",
            s.tMs / 1000.0f, s.aux0, s.v, s.i, s.v * s.i, s.temp, (int)s.aux1);
        return f.getWriteError() == 0;   // stop early if the card gave up
      });
      // replay() returning false means the flash log could not be read (or the
      // card gave up mid-stream): the datapoint block is missing or truncated,
      // and a file the header promised n samples for must not be reported —
      // let alone verified — as a good save.
      if (!logOk) return false;
    }
    return f.getWriteError() == 0;
  }, msg, msgLen);
}

inline bool saveBatt(const CapacityTest::Result &r, char *msg, size_t msgLen) {
  return sd::saveCsv("BATT", [&r](Print &f) {
    fpf(f, "# EL15 battery capacity test\n");
    writeStamp(f);
    fpf(f, "# Cutoff voltage (V),%.2f\n", r.cutoffV);
    fpf(f, "# Discharge current (A),%.3f\n", r.currentA);
    // Probe wiring changes what every voltage in this file MEANS — corrected
    // back to the pack, or measured at the load's terminals — so the file has
    // to state it. Snapshotted by the engine at start(), not read live here: a
    // Settings change between the run and a (re)save must not relabel data that
    // was recorded under the old setting.
    fpf(f, "# Probe wiring,%s\n", r.fourWire ? "4-wire (Kelvin)" : "2-wire");
    fpf(f, "# Lead resistance corrected (ohm),%.6f\n", r.fourWire ? 0.0f : r.tareOhm);
    fpf(f, "# Voltages,%s\n",
        (r.fourWire || r.tareOhm <= 0) ? "as reported by the load"
                                       : "corrected to the pack terminals (V + I*R_lead)");
    if (r.ratedAh > 0) {
      fpf(f, "# Rated capacity (Ah),%.3f\n", r.ratedAh);
      fpf(f, "# Rated capacity (mAh),%.0f\n", r.ratedAh * 1000.0f);
      fpf(f, "# Discharge rate (C),%.3f\n", r.cRate);
    }
    // The runaway guards this run armed. Recorded because a run that ends on one
    // of them reads, in the numbers alone, exactly like a battery that finished
    // there — and once did: a fixed 50 Ah cap stopped a 92 Ah pack and the report
    // gave no way to tell the difference. `stop_reason` below names the cap; these
    // two lines say what it was set to.
    fpf(f, "# Safety cap - capacity (Ah),%.1f\n", r.capAh);
    fpf(f, "# Safety cap - duration (h),%.1f\n", r.capDurationS / 3600.0f);
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
    // Battery-model figures (see battery_model.h). Written only when the run
    // established them — a chemistry with no discharge curve produces none, and
    // an invented zero would read as a measurement.
    if (r.internalResistanceOhm > 0)
      fpf(f, "internal_resistance,%.5f,ohm\n", r.internalResistanceOhm);
    if (r.startSocPct >= 0) {
      fpf(f, "start_state_of_charge,%.1f,%%\n", r.startSocPct);
      fpf(f, "end_state_of_charge,%.1f,%%\n", r.endSocPct);
    }
    // Full-pack capacity implied by Ah drawn per unit of charge state travelled.
    // Unlike `capacity` above this remains meaningful for a partial discharge.
    if (r.impliedFullAh > 0) {
      fpf(f, "implied_full_capacity,%.4f,Ah\n", r.impliedFullAh);
      fpf(f, "implied_full_capacity_mah,%.1f,mAh\n", r.impliedFullAh * 1000.0f);
    }

    // ---- Per-sample datapoints ----------------------------------------------
    // Streamed straight out of the flash log written during the run, one CSV row
    // at a time — nothing large is ever assembled in RAM. Rows carry their own
    // elapsed time because the log coarsens its sample interval on long runs
    // (see sample_log.h), so the spacing is not necessarily uniform.
    uint32_t n = samplelog::batt.count();
    fpf(f, "\n# Datapoints (%lu samples, %lu s resolution at the end of the run)\n",
        (unsigned long)n, (unsigned long)(samplelog::batt.intervalMs() / 1000));
    fpf(f, "elapsed_s,voltage_v,current_a,power_w,capacity_ah,capacity_mah,energy_wh,temperature_c\n");
    if (n > 0) {
      bool logOk = samplelog::batt.replay([&f](const samplelog::Rec &s) {
        fpf(f, "%lu,%.4f,%.4f,%.4f,%.5f,%.2f,%.4f,%.1f\n",
            (unsigned long)(s.tMs / 1000), s.v, s.i, s.v * s.i, s.aux0,
            s.aux0 * 1000.0f, s.aux1, s.temp);
        return f.getWriteError() == 0;   // stop early if the card gave up
      });
      // replay() returning false means the flash log could not be read (or the
      // card gave up mid-stream): the datapoint block is missing or truncated,
      // and a file the header promised n samples for must not be reported —
      // let alone verified — as a good save.
      if (!logOk) return false;
    }
    return f.getWriteError() == 0;
  }, msg, msgLen);
}

}  // namespace report
