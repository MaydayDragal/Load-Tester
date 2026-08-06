package com.loadtester.el15

import android.content.Context
import androidx.preference.PreferenceManager
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * CSV test reports. Kotlin port of the firmware's `report.h`.
 *
 * One file per test: a commented metadata block (what was run, when, with what
 * settings), then the computed summary, then the per-sample datapoints.
 * Spreadsheet-friendly — the '#' lines import as a single column and each block
 * has a normal header row, so the curve plots straight from the file.
 *
 * The layout is deliberately identical to the board's, down to the field names
 * and units, so a report saved from the phone and one saved from the ESP32 drop
 * into the same spreadsheet. Two honest differences from the firmware version:
 * file naming counts in SharedPreferences rather than scanning an SD card, and
 * the timestamp is always real (a phone's clock is set, so there is no
 * "(RTC not set)" case to fall back to).
 */
object Report {

    private const val SEQ_RTEST = "pref_seq_rtest"
    private const val SEQ_BATT = "pref_seq_batt"

    /** `RTEST_007.CSV` — the firmware's naming, continued. */
    fun nextName(ctx: Context, prefix: String): String {
        val key = if (prefix == "RTEST") SEQ_RTEST else SEQ_BATT
        val p = PreferenceManager.getDefaultSharedPreferences(ctx)
        val n = p.getInt(key, 0) + 1
        p.edit().putInt(key, n).apply()
        return "%s_%03d.CSV".format(Locale.US, prefix, n)
    }

    private fun StringBuilder.f(fmt: String, vararg args: Any?): StringBuilder =
        append(String.format(Locale.US, fmt, *args))

    private fun StringBuilder.stamp(): StringBuilder {
        val now = Date()
        f("# Date,%s\n", SimpleDateFormat("yyyy-MM-dd", Locale.US).format(now))
        f("# Time,%s\n", SimpleDateFormat("HH:mm:ss", Locale.US).format(now))
        f("# Timestamp,%s\n",
            SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(now))
        f("# App,EL15 Load Control for Android %s\n", BuildConfig.VERSION_NAME)
        return this
    }

    // ---- Resistance sweep ----------------------------------------------------
    fun rTestCsv(r: ResistanceTest.Result): String = buildString {
        f("# EL15 circuit resistance test\n")
        stamp()
        f("# Fuse rating (A),%.2f\n", r.fuseRating)
        f("# Sweep start current (A),%.3f\n", r.startCurrent)
        f("# Sweep peak current (A),%.3f\n", r.maxTestCurrent)
        f("# Sweep duration (s),%d\n", r.sweepSeconds)
        f("# Sweep shape,continuous triangular ramp (up then down)\n")
        f("# Probe wiring,%s\n", if (r.fourWire) "4-wire (Kelvin)" else "2-wire")
        f("# Lead tare (ohm),%.6f\n", r.tareOhm)

        // The fit runs on every raw sample; these rows are those samples averaged
        // into current bands, which is what makes a readable V-I curve. Each band
        // holds both the up-ramp and down-ramp visit to that current, so the
        // average is where the time-drift cancellation lands.
        f("\n# Averaged sample per current band (both ramp directions)\n")
        f("band,current_a,voltage_v,power_w,temperature_c,fan\n")
        r.samples.forEachIndexed { i, s ->
            f("%d,%.4f,%.4f,%.3f,%.1f,%d\n",
                i + 1, s.current, s.voltage, s.power, s.temperature, s.fanSpeed)
        }

        f("\n# Result\n")
        f("quantity,value,unit\n")
        f("resistance,%.6f,ohm\n", r.resistanceOhm)
        f("resistance_measured,%.6f,ohm\n", r.rawResistanceOhm)
        f("resistance_std_err,%.6f,ohm\n", r.resistanceStdErr)
        f("open_circuit_voltage,%.4f,V\n", r.openCircuitVoltage)
        f("r_squared,%.5f,\n", r.rSquared)
        f("raw_samples,%d,\n", r.rawSamples)
        f("current_bands,%d,\n", r.samples.size)
        f("current_min,%.4f,A\n", r.minCurrent)
        f("current_max,%.4f,A\n", r.maxCurrentSeen)
        f("voltage_sag,%.4f,V\n", r.sagV)
        f("peak_power,%.3f,W\n", r.peakPowerW)
        f("temperature_min,%.1f,C\n", r.tempMin)
        f("temperature_max,%.1f,C\n", r.tempMax)
        f("max_fan,%d,\n", r.maxFan)
        f("load_dropouts,%d,\n", r.loadDropouts)
        f("reliable,%s,\n", if (r.reliable) "yes" else "no")

        // Per-sample datapoints, streamed out of the log written during the
        // sweep. Includes the samples the fit EXCLUDED (load-off dropouts, settle
        // transients): the banded curve above is what was fitted, this block is
        // what happened. target_a is the commanded setpoint at that instant, so
        // current_a vs target_a shows the load's regulation lag; a dropout reads
        // as current_a ~ 0 under a nonzero target.
        val log = SampleLog.rtest
        f("\n# Datapoints (%d samples, %d ms resolution at the end of the sweep)\n",
            log.count, log.intervalMs)
        f("elapsed_s,target_a,voltage_v,current_a,power_w,temperature_c,fan\n")
        log.replay { s ->
            f("%.3f,%.3f,%.4f,%.4f,%.3f,%.1f,%d\n",
                s.tMs / 1000f, s.aux0, s.v, s.i, s.v * s.i, s.temp, s.aux1.toInt())
            true
        }
    }

    // ---- Battery capacity ----------------------------------------------------
    fun battCsv(r: CapacityTest.Result): String = buildString {
        f("# EL15 battery capacity test\n")
        stamp()
        f("# Cutoff voltage (V),%.2f\n", r.cutoffV)
        f("# Discharge current (A),%.3f\n", r.currentA)
        if (r.chemistry in BatteryModel.CHEMS.indices) {
            f("# Chemistry,%s\n", BatteryModel.CHEMS[r.chemistry].name)
            if (r.cells > 0) f("# Cells in series,%d\n", r.cells)
        }
        // Probe wiring changes what every voltage in this file MEANS — corrected
        // back to the pack, or measured at the load's terminals — so the file has
        // to state it. Snapshotted by the engine at start(), not read live here:
        // a Settings change between the run and a (re)save must not relabel data
        // that was recorded under the old setting.
        f("# Probe wiring,%s\n", if (r.fourWire) "4-wire (Kelvin)" else "2-wire")
        f("# Lead resistance corrected (ohm),%.6f\n", if (r.fourWire) 0f else r.tareOhm)
        f("# Voltages,%s\n",
            if (r.fourWire || r.tareOhm <= 0f) "as reported by the load"
            else "corrected to the pack terminals (V + I*R_lead)")
        if (r.ratedAh > 0f) {
            f("# Rated capacity (Ah),%.3f\n", r.ratedAh)
            f("# Rated capacity (mAh),%.0f\n", r.ratedAh * 1000f)
            f("# Discharge rate (C),%.3f\n", r.cRate)
        }
        // The runaway guards this run armed. Recorded because a run that ends on
        // one of them reads, in the numbers alone, exactly like a battery that
        // finished there — and once did: a fixed 50 Ah cap stopped a 92 Ah pack
        // and the report gave no way to tell the difference. `stop_reason` below
        // names the cap; these two lines say what it was set to.
        f("# Safety cap - capacity (Ah),%.1f\n", r.capAh)
        f("# Safety cap - duration (h),%.1f\n", r.capDurationS / 3600f)

        f("\n# Result\n")
        f("quantity,value,unit\n")
        f("capacity,%.4f,Ah\n", r.capacityAh)
        f("capacity_mah,%.1f,mAh\n", r.capacityAh * 1000f)
        f("energy,%.3f,Wh\n", r.energyWh)
        f("duration,%d,s\n", r.durationS)
        f("paused,%d,s\n", r.pausedS)
        f("start_voltage,%.3f,V\n", r.startV)
        f("end_voltage,%.3f,V\n", r.endV)
        f("rebound_voltage,%.3f,V\n", r.reboundV)
        f("average_voltage,%.3f,V\n", r.avgV)
        f("average_current,%.3f,A\n", r.avgI)
        f("average_power,%.3f,W\n", r.avgV * r.avgI)
        f("min_temperature,%.1f,C\n", r.minTemp)
        f("max_temperature,%.1f,C\n", r.maxTemp)
        f("max_fan,%d,\n", r.maxFan)
        f("stop_reason,%s,\n", r.stopReason)
        if (r.ratedAh > 0f) {
            f("rated_capacity,%.4f,Ah\n", r.ratedAh)
            f("state_of_health,%.1f,%%\n", r.sohPct)
            f("discharge_rate,%.3f,C\n", r.cRate)
        }
        // Battery-model figures. Written only when the run established them — a
        // chemistry with no discharge curve produces none, and an invented zero
        // would read as a measurement.
        if (r.internalResistanceOhm > 0f) {
            f("internal_resistance,%.5f,ohm\n", r.internalResistanceOhm)
        }
        if (r.startSocPct >= 0f) {
            f("start_state_of_charge,%.1f,%%\n", r.startSocPct)
            f("end_state_of_charge,%.1f,%%\n", r.endSocPct)
        }
        // Full-pack capacity implied by Ah drawn per unit of charge state
        // travelled. Unlike `capacity` above this remains meaningful for a
        // partial discharge.
        if (r.impliedFullAh > 0f) {
            f("implied_full_capacity,%.4f,Ah\n", r.impliedFullAh)
            f("implied_full_capacity_mah,%.1f,mAh\n", r.impliedFullAh * 1000f)
        }

        // Per-sample datapoints. Rows carry their own elapsed time because the
        // log coarsens its sample interval on long runs (see SampleLog), so the
        // spacing is not necessarily uniform.
        val log = SampleLog.batt
        f("\n# Datapoints (%d samples, %d s resolution at the end of the run)\n",
            log.count, log.intervalMs / 1000)
        f("elapsed_s,voltage_v,current_a,power_w,capacity_ah,capacity_mah,energy_wh,temperature_c\n")
        log.replay { s ->
            f("%d,%.4f,%.4f,%.4f,%.5f,%.2f,%.4f,%.1f\n",
                s.tMs / 1000, s.v, s.i, s.v * s.i, s.aux0, s.aux0 * 1000f, s.aux1, s.temp)
            true
        }
    }

    // ---- Saving ---------------------------------------------------------------
    /**
     * Write a report into Downloads. Returns the human-readable location, or null
     * if the write failed — the caller must surface that rather than claiming a
     * save that did not happen.
     */
    fun save(ctx: Context, prefix: String, csv: String): String? =
        Exporter.saveToDownloads(ctx, nextName(ctx, prefix), "text/csv", csv.toByteArray())

    fun share(ctx: Context, prefix: String, csv: String, subject: String) {
        Exporter.share(ctx, nextName(ctx, prefix), "text/csv", csv.toByteArray(), subject)
    }
}
