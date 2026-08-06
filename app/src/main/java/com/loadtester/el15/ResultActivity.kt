package com.loadtester.el15

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import com.google.android.material.chip.Chip
import com.loadtester.el15.databinding.ActivityResultBinding
import java.util.Locale

/**
 * Result screen for a finished test — the app's counterpart to the firmware's
 * result screen, for both engines.
 *
 * Shows the headline figure, the curve the run produced, every measured value,
 * and offers the CSV report. The report is byte-for-byte the same layout the
 * board writes to its SD card (see [Report]), so a phone-saved run and a
 * board-saved run drop into the same spreadsheet.
 *
 * The result is read straight off [DeviceCore], which keeps the last result of
 * each kind in memory. The app deliberately does not archive results — that was
 * the removed History feature, and the firmware never had it.
 */
class ResultActivity : BaseActivity() {

    private lateinit var binding: ActivityResultBinding
    private var kind: String = KIND_RTEST

    private var rtestResult: ResistanceTest.Result? = null
    private var battResult: CapacityTest.Result? = null

    private var pendingSave: (() -> Unit)? = null
    private val storagePermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) pendingSave?.invoke() else toast("Storage permission denied")
        pendingSave = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityResultBinding.inflate(layoutInflater)
        setContentView(binding.root)
        binding.toolbar.setNavigationOnClickListener { finish() }

        val core = DeviceCore.get(this)
        kind = intent.getStringExtra(EXTRA_KIND) ?: KIND_RTEST
        rtestResult = core.lastRTestResult
        battResult = core.lastBattResult

        // A result screen opened from a stale notification after the process was
        // rebuilt has nothing to show. Say so rather than rendering zeroes as
        // though they were a measurement.
        val have = if (kind == KIND_BATT) battResult != null else rtestResult != null
        if (!have) {
            toast(getString(R.string.result_gone))
            finish()
            return
        }

        if (kind == KIND_BATT) renderBattery() else renderRTest()

        binding.saveButton.setOnClickListener {
            withStoragePermission {
                val csv = buildCsv()
                val prefix = if (kind == KIND_BATT) "BATT" else "RTEST"
                val where = Report.save(this, prefix, csv)
                toast(
                    if (where != null) getString(R.string.rt_saved_to, where)
                    else getString(R.string.rt_save_failed)
                )
            }
        }
        binding.shareButton.setOnClickListener {
            try {
                val prefix = if (kind == KIND_BATT) "BATT" else "RTEST"
                Report.share(this, prefix, buildCsv(), getString(R.string.rt_results_title))
            } catch (e: Exception) {
                toast("Share failed: ${e.message}")
            }
        }
    }

    private fun buildCsv(): String =
        if (kind == KIND_BATT) Report.battCsv(battResult!!) else Report.rTestCsv(rtestResult!!)

    // ---- Resistance sweep ------------------------------------------------------
    private fun renderRTest() {
        val r = rtestResult ?: return
        binding.toolbar.title = getString(R.string.rt_results_title)
        binding.headlineLabel.setText(R.string.rt_circuit_resistance)

        // The uncertainty is part of the measurement, not a footnote: a 5 mohm
        // reading +/- 4 mohm and one +/- 0.1 mohm are different results.
        binding.headlineValue.text = DeviceCore.formatOhm(r.resistanceOhm)
        binding.headlineSub.text = buildString {
            if (r.resistanceStdErr > 0f) append("± ${DeviceCore.formatOhm(r.resistanceStdErr)}  ")
            append("· Voc %.3f V · R² %.4f".format(Locale.US, r.openCircuitVoltage, r.rSquared))
        }
        binding.reliabilityText.text =
            if (r.reliable) getString(R.string.rt_reliable) else getString(R.string.rt_unreliable)
        binding.reliabilityText.setTextColor(
            ContextCompat.getColor(this, if (r.reliable) R.color.value_green else R.color.value_amber)
        )

        binding.chartLabel.setText(R.string.rt_graph)
        binding.viChart.visibility = View.VISIBLE
        binding.curveChart.visibility = View.GONE
        binding.viChart.setData(r.samples, r.resistanceOhm, r.openCircuitVoltage)

        val probe = if (r.fourWire) getString(R.string.probe_4wire_short)
        else "2-wire · tare ${DeviceCore.formatOhm(r.tareOhm)}"
        binding.detailText.text = buildString {
            row("Resistance", DeviceCore.formatOhm(r.resistanceOhm))
            row("Uncertainty (1σ)", DeviceCore.formatOhm(r.resistanceStdErr))
            row("Measured (pre-tare)", DeviceCore.formatOhm(r.rawResistanceOhm))
            row("Probe wiring", probe)
            row("Open-circuit voltage", "%.4f V".format(Locale.US, r.openCircuitVoltage))
            row("R²", "%.5f".format(Locale.US, r.rSquared))
            row("Est. short-circuit current",
                if (r.resistanceOhm > 1e-6f)
                    "%.1f A".format(Locale.US, r.openCircuitVoltage / r.resistanceOhm) else "n/a")
            row("Voltage sag", "%.4f V".format(Locale.US, r.sagV))
            row("Peak power", "%.2f W".format(Locale.US, r.peakPowerW))
            row("Fuse rating", "%.2f A".format(Locale.US, r.fuseRating))
            row("Sweep", "%.3f → %.3f A over %d s".format(
                Locale.US, r.startCurrent, r.maxTestCurrent, r.sweepSeconds))
            row("Current measured", "%.4f → %.4f A".format(Locale.US, r.minCurrent, r.maxCurrentSeen))
            row("Samples fitted", "${r.rawSamples} (${r.samples.size} bands)")
            row("Temperature", "%.1f → %.1f °C".format(Locale.US, r.tempMin, r.tempMax))
            row("Max fan", "${r.maxFan}/${El15Protocol.FAN_SPEED_MAX}")
            if (r.loadDropouts > 0) row("Load dropouts", r.loadDropouts.toString())
        }
    }

    // ---- Battery capacity --------------------------------------------------------
    private fun renderBattery() {
        val r = battResult ?: return
        binding.toolbar.title = getString(R.string.batt_results_title)
        binding.headlineLabel.setText(R.string.batt_capacity)

        binding.headlineValue.text = "%.3f Ah".format(Locale.US, r.capacityAh)
        binding.headlineSub.text = "%.0f mAh · %.2f Wh · %s".format(
            Locale.US, r.capacityAh * 1000f, r.energyWh, DeviceCore.formatDuration(r.durationS))
        binding.reliabilityText.text = r.stopReason
        binding.reliabilityText.setTextColor(
            ContextCompat.getColor(
                this,
                // A run that ended on a safety cap is not a measurement of the
                // pack — it is the guard firing. Colour it as a caution.
                if (r.stopReason.contains("cap", ignoreCase = true) ||
                    r.stopReason.contains("Protection", ignoreCase = true)
                ) R.color.value_amber else R.color.value_green
            )
        )

        binding.chartLabel.setText(R.string.batt_curve)
        binding.viChart.visibility = View.GONE
        binding.curveChart.visibility = View.VISIBLE
        binding.seriesGroup.visibility = View.VISIBLE

        // Rebuild the discharge curve from the engine's datapoint log.
        val pts = ArrayList<TimeSeriesChartView.TimePoint>()
        SampleLog.batt.replay { s ->
            pts.add(TimeSeriesChartView.TimePoint(
                s.tMs, s.v, s.i, s.v * s.i, s.temp, s.aux0, s.aux1))
            true
        }
        binding.curveChart.setData(pts)

        for (series in binding.curveChart.seriesList()) {
            val chip = layoutInflater.inflate(R.layout.chip_mode, binding.seriesGroup, false) as Chip
            chip.text = series.name
            chip.id = View.generateViewId()
            chip.isCheckable = true
            chip.isChecked = binding.curveChart.isSeriesOn(series.key)
            chip.setOnClickListener {
                binding.curveChart.toggleSeries(series.key)
                chip.isChecked = binding.curveChart.isSeriesOn(series.key)
            }
            binding.seriesGroup.addView(chip)
        }

        val chemName = BatteryModel.CHEMS.getOrNull(r.chemistry)?.name
        binding.detailText.text = buildString {
            row("Capacity", "%.4f Ah  (%.0f mAh)".format(Locale.US, r.capacityAh, r.capacityAh * 1000f))
            row("Energy", "%.3f Wh".format(Locale.US, r.energyWh))
            row("Duration", DeviceCore.formatDuration(r.durationS))
            if (r.pausedS > 0) row("Paused", DeviceCore.formatDuration(r.pausedS))
            row("Stop reason", r.stopReason)
            if (chemName != null) {
                row("Chemistry", chemName + if (r.cells > 0) " · ${r.cells}S" else "")
            }
            row("Discharge current", "%.3f A".format(Locale.US, r.currentA))
            row("Cutoff voltage", "%.2f V".format(Locale.US, r.cutoffV))
            row("Start voltage", "%.3f V".format(Locale.US, r.startV))
            row("End voltage", "%.3f V".format(Locale.US, r.endV))
            row("Rebound voltage", "%.3f V".format(Locale.US, r.reboundV))
            row("Average", "%.3f V @ %.3f A  (%.2f W)".format(
                Locale.US, r.avgV, r.avgI, r.avgV * r.avgI))
            // Only written when the run actually established them — an invented
            // zero would read as a measurement.
            if (r.ratedAh > 0f) {
                row("Rated capacity", "%.3f Ah".format(Locale.US, r.ratedAh))
                row("State of health", "%.1f %%".format(Locale.US, r.sohPct))
                row("Discharge rate", "%.3f C".format(Locale.US, r.cRate))
            }
            if (r.internalResistanceOhm > 0f) {
                row("Pack resistance", DeviceCore.formatOhm(r.internalResistanceOhm))
            }
            if (r.startSocPct >= 0f) {
                row("Charge state", "~%.0f %% → ~%.0f %%".format(
                    Locale.US, r.startSocPct, r.endSocPct))
            }
            if (r.impliedFullAh > 0f) {
                row("Implied full capacity", "%.3f Ah".format(Locale.US, r.impliedFullAh))
            }
            row("Probe wiring",
                if (r.fourWire) getString(R.string.probe_4wire_short)
                else "2-wire · tare ${DeviceCore.formatOhm(r.tareOhm)}")
            row("Temperature", "%.1f → %.1f °C".format(Locale.US, r.minTemp, r.maxTemp))
            row("Max fan", "${r.maxFan}/${El15Protocol.FAN_SPEED_MAX}")
            row("Safety caps", "%.1f Ah · %.1f h".format(
                Locale.US, r.capAh, r.capDurationS / 3600f))
        }
    }

    /** Aligned "label  value" line for the detail block. */
    private fun StringBuilder.row(label: String, value: String) {
        append(label.padEnd(24)).append(value).append('\n')
    }

    private fun withStoragePermission(action: () -> Unit) {
        if (!Exporter.needsLegacyWritePermission() ||
            ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
            PackageManager.PERMISSION_GRANTED
        ) {
            action()
        } else {
            pendingSave = action
            storagePermission.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        }
    }

    private fun toast(msg: String) =
        android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_LONG).show()

    companion object {
        const val EXTRA_KIND = "kind"
        const val KIND_RTEST = "rtest"
        const val KIND_BATT = "batt"
    }
}
