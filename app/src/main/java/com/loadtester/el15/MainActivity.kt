package com.loadtester.el15

import android.Manifest
import android.annotation.SuppressLint
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.os.Build
import android.os.Bundle
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.View
import android.view.WindowManager
import android.widget.ArrayAdapter
import android.widget.LinearLayout
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.core.content.ContextCompat
import com.google.android.material.chip.Chip
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import com.loadtester.el15.databinding.ActivityMainBinding
import java.util.Locale
import kotlin.math.ceil

/**
 * The instrument panel. All device/session state lives in [DeviceCore] — this
 * activity is a view over the core (it can be destroyed and recreated freely
 * while a connection, sweep, or bench session keeps running underneath).
 */
class MainActivity : BaseActivity(), DeviceCore.Ui {

    private lateinit var binding: ActivityMainBinding
    private lateinit var core: DeviceCore

    private var userEditingSetpoint = false
    private var updatingChips = false
    private var maxVoltageSeen = 0f

    private var pendingThemeRecreate = false
    private var nightMode = -1
    private var pendingResultId: String? = null
    private var pendingResultSession = false

    /** Which control panel is showing under the mode chips. */
    private enum class Panel { DEVICE, RTEST, BENCH }
    private var panel = Panel.DEVICE

    // Only the six device modes map to chips that send a mode command.
    private val chipToMode: Map<Int, Int> by lazy {
        with(binding.monitor) {
            mapOf(
                chipCc.id to El15Protocol.MODE_CC, chipCv.id to El15Protocol.MODE_CV,
                chipCr.id to El15Protocol.MODE_CR, chipCp.id to El15Protocol.MODE_CP,
                chipCap.id to El15Protocol.MODE_CAP, chipDcr.id to El15Protocol.MODE_DCR,
            )
        }
    }

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result.values.all { it }) startBleScan()
        else toast("Bluetooth permission denied — the demo device is still available")
    }

    private val notifPermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { /* optional — alarms/results notifications simply stay silent if denied */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        core = DeviceCore.get(this)
        core.syncSettings()
        nightMode = resources.configuration.uiMode and
            android.content.res.Configuration.UI_MODE_NIGHT_MASK

        binding.headerHistory.setOnClickListener { startActivity(Intent(this, HistoryActivity::class.java)) }
        binding.headerSettings.setOnClickListener { startActivity(Intent(this, SettingsActivity::class.java)) }
        binding.headerInfo.setOnClickListener { showAbout() }

        with(binding.monitor) {
            gaugeVoltage.arcColor = color(R.color.value_green); gaugeVoltage.unit = "V"
            gaugeCurrent.arcColor = color(R.color.value_amber); gaugeCurrent.unit = "A"
        }
        setupControls()
        setupModeChips()
        setupWaveformControls()
        setupTest()
        setupBench()
        applyLegend()
        clearReadouts()

        core.addUi(this)
        renderConnectionState()
        restoreLiveState()

        if (Build.VERSION.SDK_INT >= 33 && !Notifications.canPost(this)) {
            notifPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        // The core (and its foreground service) owns the session — never tear
        // down the connection just because the Activity died.
        if (::core.isInitialized) core.removeUi(this)
    }

    /** Rebuild transient view state from the core after (re)creation. */
    private fun restoreLiveState() = with(binding.monitor) {
        for (pt in core.waveLog) waveformView.add(pt.v, pt.i, pt.p, pt.temp, pt.fan, pt.tMs)
        if (core.waveLog.isNotEmpty()) waveStats.text = waveformView.statsText()
        core.lastStatus?.let { if (core.isConnected) renderStatus(it) }
        if (core.tester.running) {
            panel = Panel.RTEST
            testBar.visibility = View.VISIBLE
            testProgressText.visibility = View.VISIBLE
            testProgressText.text = core.lastProgressText
            startTestButton.setText(R.string.rt_stop)
        } else if (core.calibrator.running) {
            panel = Panel.RTEST
            testProgressText.visibility = View.VISIBLE
            testProgressText.text = core.lastProgressText
        } else if (core.session.running) {
            panel = Panel.BENCH
            benchProgressText.visibility = View.VISIBLE
            benchProgressText.text = core.lastProgressText
        }
        applyPanel()
        updateRecordIcon()
    }

    override fun onResume() {
        super.onResume()
        pendingResultId?.let { id ->
            val session = pendingResultSession
            pendingResultId = null
            if (session) SessionResultActivity.start(this, id)
            else startActivity(Intent(this, ResultActivity::class.java)
                .putExtra(ResultActivity.EXTRA_RECORD_ID, id))
        }
        core.syncSettings()
        // Demo circuit may have been edited in Settings; apply it live.
        val newEmf = Prefs.demoEmf(this); val newR = Prefs.demoR(this)
        if (newEmf != core.demoEmf || newR != core.demoSeriesR) {
            core.applyDemoCircuit(newEmf, newR)
            if (core.isDemo) updateDemoStatusText()
        }
        if (Prefs.keepScreenOn(this)) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        // Only a real day/night flip triggers a re-theme, and never mid-session.
        val newNight = newConfig.uiMode and android.content.res.Configuration.UI_MODE_NIGHT_MASK
        if (newNight == nightMode) return
        nightMode = newNight
        if (!core.isConnected && !core.busy) recreate() else pendingThemeRecreate = true
    }

    private fun color(res: Int) = ContextCompat.getColor(this, res)

    private fun showAbout() = MaterialAlertDialogBuilder(this)
        .setTitle(getString(R.string.app_name))
        .setMessage(getString(R.string.about_body, BuildConfig.VERSION_NAME))
        .setPositiveButton(android.R.string.ok, null).show()

    private fun deviceLabel(): String =
        if (core.isDemo) "Demo (%.1f V, %.3f Ω)".format(core.demoEmf, core.demoSeriesR)
        else "EL15 (BLE)"

    // ---- Controls ---------------------------------------------------------
    private fun setupControls() = with(binding.monitor) {
        connButton.setOnClickListener {
            if (core.isConnected || core.ble.state == El15BleManager.State.CONNECTING) core.disconnect()
            else requestScan()
        }
        connStatusTap.setOnClickListener {
            if (core.isDemo) showDemoConfigDialog(applyLive = true, onDone = null)
        }
        setpointInput.setOnFocusChangeListener { _, f -> userEditingSetpoint = f }
        setSetpointButton.setOnClickListener {
            val v = setpointInput.text.toString().trim().toFloatOrNull()
            if (v == null) toast("Enter a valid number")
            else { core.controller.setSetpoint(v); setpointInput.clearFocus(); toast("Setpoint → $v") }
        }
        loadToggle.setOnClickListener {
            if (!core.isConnected) requestScan()
            else core.controller.setLoad(!(core.lastStatus?.loadOn ?: false))
        }
        lockButton.setOnClickListener { core.controller.setLock() }
    }

    private fun setupModeChips() = with(binding.monitor) {
        for (id in chipToMode.keys) findViewById<Chip>(id).setOnClickListener {
            if (updatingChips) return@setOnClickListener
            panel = Panel.DEVICE
            applyPanel()
            chipToMode[id]?.let { core.controller.setMode(it) }
        }
        fun panelChip(chip: Chip, target: Panel, clearLoad: Boolean) {
            chip.setOnClickListener {
                if (updatingChips) return@setOnClickListener
                if (panel == target && !chip.isChecked) {
                    // Re-tap on the active chip: keep it selected, panel stays.
                    updatingChips = true; chip.isChecked = true; updatingChips = false
                    return@setOnClickListener
                }
                panel = target
                if (clearLoad && core.isConnected && !core.busy) core.controller.setLoad(false)
                applyPanel()
            }
        }
        panelChip(chipRtest, Panel.RTEST, clearLoad = true)
        panelChip(chipBench, Panel.BENCH, clearLoad = false)
    }

    private fun applyPanel() = with(binding.monitor) {
        deviceControls.visibility = if (panel == Panel.DEVICE) View.VISIBLE else View.GONE
        rtestPanel.visibility = if (panel == Panel.RTEST) View.VISIBLE else View.GONE
        benchPanel.visibility = if (panel == Panel.BENCH) View.VISIBLE else View.GONE
        if (panel != Panel.DEVICE) {
            updatingChips = true
            modeChipGroup.check(if (panel == Panel.RTEST) chipRtest.id else chipBench.id)
            updatingChips = false
        }
        updateControlsEnabled()
    }

    private fun setupWaveformControls() = with(binding.monitor) {
        wavePauseButton.setOnClickListener {
            waveformView.paused = !waveformView.paused
            wavePauseButton.setIconResource(if (waveformView.paused) R.drawable.ic_play else R.drawable.ic_pause)
        }
        waveRecordButton.setOnClickListener {
            // Recording lives in the core so it survives the Activity.
            if (core.recording) {
                core.stopRecording()
                toast("Recording stopped (${core.recordLog.size} samples)")
            } else {
                core.startRecording()
                toast("Recording… (kept while the app runs in background)")
            }
            updateRecordIcon()
        }
        waveExportButton.setOnClickListener { waveformExportChooser() }
    }

    private fun updateRecordIcon() = with(binding.monitor) {
        waveRecordButton.alpha = if (core.recording) 1f else 0.55f
    }

    private fun applyLegend() {
        val sb = SpannableStringBuilder()
        sb.append("● Voltage")
        sb.setSpan(ForegroundColorSpan(color(R.color.value_green)), 0, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        val s = sb.length
        sb.append("    ● Current")
        sb.setSpan(ForegroundColorSpan(color(R.color.value_amber)), s, sb.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        binding.monitor.waveLegend.text = sb
    }

    // ---- Resistance test ----------------------------------------------------
    private fun setupTest() = with(binding.monitor) {
        if (stepsInput.text.isNullOrBlank()) stepsInput.setText(Prefs.steps(this@MainActivity).toString())
        if (settleInput.text.isNullOrBlank()) settleInput.setText(Prefs.settleMs(this@MainActivity).toString())
        if (sampleInput.text.isNullOrBlank()) sampleInput.setText(Prefs.sampleMs(this@MainActivity).toString())

        fun info(layout: TextInputLayout, title: Int, body: Int) {
            layout.setEndIconOnClickListener {
                MaterialAlertDialogBuilder(this@MainActivity)
                    .setTitle(title).setMessage(body)
                    .setPositiveButton(android.R.string.ok, null).show()
            }
        }
        info(stepsLayout, R.string.rt_steps, R.string.rt_steps_info)
        info(settleLayout, R.string.rt_settle, R.string.rt_settle_info)
        info(sampleLayout, R.string.rt_sample, R.string.rt_sample_info)

        calibrateButton.setOnClickListener {
            if (core.calibrator.running) {
                core.calibrator.stop()
                onTestFinishedUi("Calibration stopped")
                return@setOnClickListener
            }
            if (!core.isConnected) { toast("Connect to the EL15 (or the demo) first"); return@setOnClickListener }
            val fuse = fuseInput.text.toString().trim().toFloatOrNull()
            if (fuse == null || fuse <= 0f) { toast("Enter the circuit's fuse rating in amps"); return@setOnClickListener }
            if (fuse > 200f) { toast("That fuse rating looks too high — double-check it"); return@setOnClickListener }
            val (peak, _) = predictedPeak(fuse)
            val poll = Prefs.pollMs(this@MainActivity)
            val estS = (12_000L + 3 * (2 * poll + 300) + 25 * 2300L) / 1000L
            val estStr = if (estS >= 120) "~${estS / 60} min" else "~${estS}s"
            MaterialAlertDialogBuilder(this@MainActivity)
                .setTitle(R.string.rt_cal_confirm_title)
                .setMessage(getString(R.string.rt_cal_confirm_msg, peak * 0.5f, estStr))
                .setPositiveButton(R.string.rt_calibrate) { _, _ -> startCalibration(fuse) }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }
        startTestButton.setOnClickListener {
            if (core.tester.running) { core.tester.stop(); onTestFinishedUi("Test stopped"); return@setOnClickListener }
            if (!core.isConnected) { toast("Connect to the EL15 (or the demo) first"); return@setOnClickListener }
            val fuse = fuseInput.text.toString().trim().toFloatOrNull()
            if (fuse == null || fuse <= 0f) { toast("Enter the circuit's fuse rating in amps"); return@setOnClickListener }
            if (fuse > 200f) { toast("That fuse rating looks too high — double-check it"); return@setOnClickListener }
            if (!applyTestOptions()) return@setOnClickListener
            val (peak, limiter) = predictedPeak(fuse)
            MaterialAlertDialogBuilder(this@MainActivity)
                .setTitle(R.string.rt_confirm_title)
                .setMessage(getString(R.string.rt_confirm_msg, peak, limiter))
                .setPositiveButton(R.string.rt_start) { _, _ -> startTest(fuse) }
                .setNegativeButton(R.string.cancel, null).show()
        }
    }

    private fun applyTestOptions(): Boolean = with(binding.monitor) {
        val steps = stepsInput.text.toString().trim().toIntOrNull() ?: Prefs.steps(this@MainActivity)
        val settle = settleInput.text.toString().trim().toLongOrNull() ?: Prefs.settleMs(this@MainActivity)
        val sample = sampleInput.text.toString().trim().toLongOrNull() ?: Prefs.sampleMs(this@MainActivity)
        if (steps !in 2..CircuitResistanceTester.MAX_STEPS) {
            toast("Steps must be 2–${CircuitResistanceTester.MAX_STEPS}"); return false
        }
        if (settle < 0) { toast("Settle time can't be negative"); return false }
        if (sample < 0) { toast("Sample window can't be negative"); return false }
        core.tester.steps = steps; core.tester.settleMs = settle; core.tester.collectMs = sample
        stepsInput.setText(steps.toString()); settleInput.setText(settle.toString()); sampleInput.setText(sample.toString())
        true
    }

    private fun predictedPeak(fuse: Float): Pair<Float, String> {
        val v = core.lastStatus?.voltage?.takeIf { it > El15Protocol.MIN_VOLTAGE_V } ?: El15Protocol.MAX_VOLTAGE_V
        val fuseCap = fuse * core.tester.safetyFactor
        val powerCap = El15Protocol.MAX_POWER_W / v
        val peak = minOf(fuseCap, powerCap, El15Protocol.MAX_CURRENT_A)
        val limiter = when (peak) {
            fuseCap -> "%.0f%% of the %.1f A fuse".format(core.tester.safetyFactor * 100, fuse)
            powerCap -> "the %.0f W power limit at %.1f V".format(El15Protocol.MAX_POWER_W, v)
            else -> "the %.0f A current limit".format(El15Protocol.MAX_CURRENT_A)
        }
        return peak to limiter
    }

    private fun startTest(fuse: Float) = with(binding.monitor) {
        fuseInput.clearFocus()
        testBar.visibility = View.VISIBLE; testBar.progress = 0
        testProgressText.visibility = View.VISIBLE
        val poll = Prefs.pollMs(this@MainActivity)
        val perStep = maxOf(core.tester.settleMs, poll + 100) + maxOf(core.tester.collectMs, 2 * poll + 200)
        val estS = core.tester.steps.toLong() * perStep / 1000L
        val estStr = if (estS >= 120) "~${estS / 60}m ${estS % 60}s" else "~${estS}s"
        testProgressText.text = "Priming… ($estStr)"
        startTestButton.setText(R.string.rt_stop)
        core.tester.start(fuse) // start FIRST so tester.running locks the controls below
        updateControlsEnabled()
    }

    private fun startCalibration(fuse: Float) = with(binding.monitor) {
        fuseInput.clearFocus()
        testBar.visibility = View.GONE
        testProgressText.visibility = View.VISIBLE
        testProgressText.text = "Calibrating…"
        core.calibrator.start(fuse)
        updateControlsEnabled()
    }

    private fun onTestFinishedUi(message: String?) = with(binding.monitor) {
        startTestButton.setText(R.string.rt_start)
        testBar.visibility = View.GONE
        message?.let { testProgressText.visibility = View.VISIBLE; testProgressText.text = it }
        updateControlsEnabled()
    }

    // ---- DeviceCore.Ui: resistance test / calibration -----------------------
    override fun coreTestProgress(text: String, percent: Int) = with(binding.monitor) {
        if (percent >= 0) {
            testBar.visibility = View.VISIBLE
            testBar.progress = percent.coerceIn(0, 100)
        }
        testProgressText.visibility = View.VISIBLE
        testProgressText.text = text
    }

    override fun coreTestComplete(recordId: String, summary: String) {
        onTestFinishedUi("Done — R = $summary")
        openResult(recordId, session = false)
    }

    override fun coreTestError(message: String) {
        onTestFinishedUi(null)
        binding.monitor.testProgressText.visibility = View.VISIBLE
        binding.monitor.testProgressText.text = message
        if (lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED)) {
            MaterialAlertDialogBuilder(this).setTitle("Test stopped").setMessage(message)
                .setPositiveButton(android.R.string.ok, null).show()
        }
    }

    override fun coreCalComplete(result: SweepCalibrator.CalResult) {
        onTestFinishedUi("Calibration done — recommended ${result.steps} steps")
        if (!lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED)) return
        val sweeps = result.sweepResistances.joinToString(" → ") { formatOhm(it) }
        val body = StringBuilder()
            .append("Steps: ${result.steps}")
            .append(if (result.converged) "  (resistance converged: $sweeps)" else "  (did not fully converge: $sweeps — using extra headroom)")
            .append("\n\nSettle: ${result.settleMs} ms  (measured stabilization)")
            .append("\nSample: ${result.sampleMs} ms  (noise σ %.1f mV)".format(result.noiseVolts * 1000f))
            .append("\n\nEstimated test duration: ~${result.estimatedTestS}s")
            .toString()
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.rt_cal_result_title)
            .setMessage(body)
            .setPositiveButton(R.string.rt_cal_apply) { _, _ ->
                with(binding.monitor) {
                    stepsInput.setText(result.steps.toString())
                    settleInput.setText(result.settleMs.toString())
                    sampleInput.setText(result.sampleMs.toString())
                }
                toast("Sweep settings applied")
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun openResult(recordId: String, session: Boolean) {
        if (lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED)) {
            if (session) SessionResultActivity.start(this, recordId)
            else startActivity(Intent(this, ResultActivity::class.java)
                .putExtra(ResultActivity.EXTRA_RECORD_ID, recordId))
        } else {
            // Background activity starts are blocked; open it when we return.
            pendingResultId = recordId
            pendingResultSession = session
        }
    }

    // ---- Bench sessions -------------------------------------------------------
    private data class BField(val key: String, val label: String, val def: String)

    private fun setupBench() = with(binding.monitor) {
        benchCapacity.setOnClickListener { benchGuard { startCapacityFlow() } }
        benchRuntime.setOnClickListener { benchGuard { startRuntimeFlow() } }
        benchStep.setOnClickListener { benchGuard { startStepFlow() } }
        benchOcp.setOnClickListener { benchGuard { startOcpFlow() } }
        benchStopButton.setOnClickListener {
            core.session.stop()
            onBenchFinishedUi("Stopped")
        }
    }

    private fun benchGuard(start: () -> Unit) {
        when {
            !core.isConnected -> toast("Connect to the EL15 (or the demo) first")
            core.busy -> toast("Another test is already running")
            else -> start()
        }
    }

    /** Numeric-entry dialog used by all four bench test setups. */
    private fun benchDialog(
        title: String, message: String, fields: List<BField>,
        onOk: (LinkedHashMap<String, Float>) -> Unit,
    ) {
        val pad = (resources.displayMetrics.density * 20).toInt()
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad / 2, pad, 0)
        }
        val editors = LinkedHashMap<String, TextInputEditText>()
        for (f in fields) {
            val layout = TextInputLayout(this, null,
                com.google.android.material.R.attr.textInputOutlinedStyle).apply {
                hint = f.label
            }
            val edit = TextInputEditText(layout.context).apply {
                setText(f.def)
                inputType = android.text.InputType.TYPE_CLASS_NUMBER or
                    android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL
                typeface = android.graphics.Typeface.MONOSPACE
            }
            layout.addView(edit)
            val lp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
            lp.topMargin = pad / 3
            container.addView(layout, lp)
            editors[f.key] = edit
        }
        MaterialAlertDialogBuilder(this)
            .setTitle(title)
            .setMessage(message)
            .setView(container)
            .setPositiveButton(R.string.bench_next) { _, _ ->
                val values = LinkedHashMap<String, Float>()
                for ((key, edit) in editors) {
                    val v = edit.text.toString().trim().toFloatOrNull()
                    if (v == null || v < 0f) { toast("Invalid value for ${fields.first { it.key == key }.label}"); return@setPositiveButton }
                    values[key] = v
                }
                onOk(values)
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun confirmBench(
        type: Int, params: LinkedHashMap<String, Float>, summary: String, tag: String = "",
    ) {
        val limit = params.remove("limitA") ?: El15Protocol.MAX_CURRENT_A
        if (limit <= 0f) { toast("Current limit must be positive"); return }
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.bench_confirm_title)
            .setMessage(getString(R.string.bench_confirm_msg, summary,
                minOf(limit * core.session.safetyFactor, El15Protocol.MAX_CURRENT_A)))
            .setPositiveButton(R.string.rt_start) { _, _ ->
                onBenchStartedUi()
                core.session.start(type, limit, params, deviceLabel(), tag)
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    /** Step 1: pick a known battery (auto-fills everything) or go custom. */
    private fun startCapacityFlow() {
        val batteries = LongTestEngine.BATTERIES
        val labels = (batteries.map { it.name } +
            getString(R.string.bench_capacity_custom)).toTypedArray()
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.bench_capacity_battery)
            .setItems(labels) { _, which ->
                if (which >= batteries.size) chooseChemistryThenCapacity()
                else batteries[which].let {
                    capacityDialog(it.chemIndex, it.cells, it.ratedAh, it.cutoffV, it.defaultCRate, it.name)
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    /** Custom path: choose chemistry, then seed the dialog from the live OCV. */
    private fun chooseChemistryThenCapacity() {
        val chems = LongTestEngine.CHEMISTRIES
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.bench_capacity_chem)
            .setItems(chems.map { it.name }.toTypedArray()) { _, which ->
                val chem = chems[which]
                val voc = core.lastStatus?.voltage ?: 0f
                val cells = LongTestEngine.estimateCells(voc, chem)
                capacityDialog(which, cells, 0f, cells * chem.cutoffPerCell, 0.2f, "")
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    /**
     * Friendly capacity setup: pre-filled fields, C-rate quick-set from the
     * rated capacity, a live "≈ duration / C-rate / stops at" summary, and a
     * safe-floor warning if the stop voltage is dropped below the chemistry
     * minimum. The battery name becomes the record's tag for History/Trends.
     */
    private fun capacityDialog(
        chemIndex: Int, cells: Int, ratedAh: Float, cutoffV: Float, cRate: Float, presetName: String,
    ) {
        val chem = LongTestEngine.CHEMISTRIES[chemIndex]
        val pad = (resources.displayMetrics.density * 20).toInt()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad / 2, pad, 0)
        }

        fun field(hint: String, value: String): TextInputEditText {
            val layout = TextInputLayout(this, null,
                com.google.android.material.R.attr.textInputOutlinedStyle).apply { this.hint = hint }
            val edit = TextInputEditText(layout.context).apply {
                setText(value)
                inputType = android.text.InputType.TYPE_CLASS_NUMBER or
                    android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL
                typeface = android.graphics.Typeface.MONOSPACE
            }
            layout.addView(edit)
            val lp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
            lp.topMargin = pad / 3
            column.addView(layout, lp)
            return edit
        }

        val startDischarge = if (ratedAh > 0f && cRate > 0f)
            minOf(ratedAh * cRate, El15Protocol.MAX_CURRENT_A) else 1.0f
        val cellsEdit = field(getString(R.string.cap_cells), cells.toString())
        val ratedEdit = field(getString(R.string.cap_rated), if (ratedAh > 0f) fmtField(ratedAh) else "0")
        val currentEdit = field(getString(R.string.cap_current),
            String.format(Locale.US, "%.2f", startDischarge))
        val cutoffEdit = field(getString(R.string.cap_stop_v), String.format(Locale.US, "%.2f", cutoffV))
        val limitEdit = field(getString(R.string.cap_limit), "12")

        // C-rate quick-set chips: current = C × rated capacity.
        val chipRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        column.addView(chipRow, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
            .apply { topMargin = pad / 3 })
        val summary = android.widget.TextView(this).apply {
            setTextColor(color(R.color.muted)); textSize = 12f
        }
        column.addView(summary, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
            .apply { topMargin = pad / 3 })

        fun refreshSummary() {
            val rated = ratedEdit.text.toString().trim().toFloatOrNull() ?: 0f
            val cur = currentEdit.text.toString().trim().toFloatOrNull() ?: 0f
            val cutoff = cutoffEdit.text.toString().trim().toFloatOrNull() ?: 0f
            val n = cellsEdit.text.toString().trim().toIntOrNull() ?: cells
            val parts = ArrayList<String>()
            if (rated > 0f && cur > 0f) {
                val hours = rated / cur
                parts += "≈ ${SessionRecord.fmtDuration((hours * 3600).toLong())} at %.2fC".format(cur / rated)
            }
            if (cutoff > 0f) parts += "stops at %.2f V".format(cutoff)
            val floor = n * chem.cutoffPerCell
            var warn = if (cutoff in 0.01f..floor && cutoff < floor)
                "\n⚠ below the %s safe floor of %.2f V".format(chem.name, floor) else ""
            val liveV = core.lastStatus?.voltage ?: 0f
            if (cutoff > 0f && liveV > El15Protocol.MIN_VOLTAGE_V && cutoff >= liveV) {
                warn += "\n⚠ battery reads %.2f V now — the test would stop immediately".format(liveV)
            }
            summary.text = (if (parts.isEmpty()) getString(R.string.cap_enter_hint)
            else parts.joinToString(" · ")) + warn
        }

        for (c in listOf(0.2f, 0.5f, 1.0f)) {
            val btn = com.google.android.material.button.MaterialButton(this,
                null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = if (c == 1.0f) "1C" else "%.1fC".format(c)
                setOnClickListener {
                    val rated = ratedEdit.text.toString().trim().toFloatOrNull() ?: 0f
                    if (rated <= 0f) { toast(getString(R.string.cap_need_rated)); return@setOnClickListener }
                    currentEdit.setText(String.format(Locale.US, "%.2f",
                        minOf(rated * c, El15Protocol.MAX_CURRENT_A)))
                }
            }
            chipRow.addView(btn, LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply { marginEnd = pad / 4 })
        }

        for (e in listOf(cellsEdit, ratedEdit, currentEdit, cutoffEdit)) {
            e.addTextChangedListener(object : android.text.TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: android.text.Editable?) = refreshSummary()
            })
        }
        refreshSummary()

        val scroller = android.widget.ScrollView(this).apply { addView(column) }
        val title = if (presetName.isNotEmpty()) presetName else getString(R.string.bench_capacity)
        MaterialAlertDialogBuilder(this)
            .setTitle(title)
            .setView(scroller)
            .setPositiveButton(R.string.bench_next) { _, _ ->
                val p = LinkedHashMap<String, Float>()
                val n = cellsEdit.text.toString().trim().toIntOrNull()
                val cur = currentEdit.text.toString().trim().toFloatOrNull()
                val cutoff = cutoffEdit.text.toString().trim().toFloatOrNull()
                val rated = ratedEdit.text.toString().trim().toFloatOrNull() ?: 0f
                val limit = limitEdit.text.toString().trim().toFloatOrNull()
                if (n == null || n < 1) { toast("Enter cells in series"); return@setPositiveButton }
                if (cur == null || cur <= 0f) { toast("Enter a discharge current"); return@setPositiveButton }
                if (cutoff == null || cutoff <= 0f) { toast("Enter a stop voltage"); return@setPositiveButton }
                if (limit == null || limit <= 0f) { toast("Enter a current limit"); return@setPositiveButton }
                p["cells"] = n.toFloat()
                p["dischargeA"] = cur
                p["cutoffV"] = cutoff
                p["ratedAh"] = rated
                p["chem"] = chemIndex.toFloat()
                p["limitA"] = limit
                confirmBench(SessionRecord.TYPE_CAPACITY, p,
                    "CC discharge at %.2f A until %.2f V".format(cur, cutoff), presetName)
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun startRuntimeFlow() {
        val voc = core.lastStatus?.voltage ?: 0f
        benchDialog(
            getString(R.string.bench_runtime),
            getString(R.string.bench_runtime_msg),
            listOf(
                BField("powerW", "Constant power (W)", "10"),
                BField("cutoffV", "Cutoff voltage (V)",
                    if (voc > 1f) String.format(Locale.US, "%.2f", voc * 0.75f) else "9.0"),
                BField("limitA", "Current limit (A)", "12"),
            )) { p ->
            confirmBench(SessionRecord.TYPE_RUNTIME, p,
                "CP load of %.1f W until %.2f V".format(p["powerW"], p["cutoffV"]))
        }
    }

    private fun startStepFlow() {
        benchDialog(
            getString(R.string.bench_step),
            getString(R.string.bench_step_msg),
            listOf(
                BField("iLow", "Low current (A)", "0.2"),
                BField("iHigh", "High current (A)", "1.0"),
                BField("periodMs", "Period (ms)", "2000"),
                BField("cycles", "Cycles", "10"),
                BField("limitA", "Current limit (A)", "12"),
            )) { p ->
            if ((p["iHigh"] ?: 0f) <= (p["iLow"] ?: 0f)) { toast("High current must exceed low current"); return@benchDialog }
            confirmBench(SessionRecord.TYPE_STEP, p,
                "Square wave %.2f ↔ %.2f A, %.0f cycles".format(p["iLow"], p["iHigh"], p["cycles"]))
        }
    }

    private fun startOcpFlow() {
        benchDialog(
            getString(R.string.bench_ocp),
            getString(R.string.bench_ocp_msg),
            listOf(
                BField("startA", "Start current (A)", "0.2"),
                BField("stepA", "Step size (A)", "0.1"),
                BField("dwellMs", "Dwell per step (ms)", "1500"),
                BField("collapsePct", "Collapse threshold (% of Voc)", "70"),
                BField("limitA", "Current limit (A)", "12"),
            )) { p ->
            val pct = (p.remove("collapsePct") ?: 70f).coerceIn(10f, 95f)
            p["collapseFrac"] = pct / 100f
            if ((p["stepA"] ?: 0f) <= 0f) { toast("Step size must be positive"); return@benchDialog }
            confirmBench(SessionRecord.TYPE_OCP, p,
                "Ramp from %.2f A in %.2f A steps until the voltage collapses".format(p["startA"], p["stepA"]))
        }
    }

    private fun onBenchStartedUi() = with(binding.monitor) {
        benchStopButton.visibility = View.VISIBLE
        benchProgressText.visibility = View.VISIBLE
        benchProgressText.text = "Priming…"
        updateControlsEnabled()
    }

    private fun onBenchFinishedUi(message: String?) = with(binding.monitor) {
        benchStopButton.visibility = View.GONE
        message?.let { benchProgressText.visibility = View.VISIBLE; benchProgressText.text = it }
        updateControlsEnabled()
    }

    // ---- DeviceCore.Ui: bench sessions ----------------------------------------
    override fun coreSessionProgress(text: String) = with(binding.monitor) {
        benchStopButton.visibility = View.VISIBLE
        benchProgressText.visibility = View.VISIBLE
        benchProgressText.text = text
    }

    override fun coreSessionComplete(recordId: String, summary: String) {
        onBenchFinishedUi("Done — $summary")
        openResult(recordId, session = true)
    }

    override fun coreSessionError(message: String) {
        onBenchFinishedUi(message)
        if (lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED)) {
            MaterialAlertDialogBuilder(this).setTitle("Test stopped").setMessage(message)
                .setPositiveButton(android.R.string.ok, null).show()
        }
    }

    private fun formatOhm(ohm: Float) = when {
        ohm <= 0f -> "n/a"; ohm < 1f -> "%.1f mΩ".format(ohm * 1000f); else -> "%.3f Ω".format(ohm)
    }

    // ---- Permissions / scanning ------------------------------------------
    private fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

    private fun hasPermissions() = requiredPermissions().all {
        ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
    }

    private fun requestScan() {
        showDevicePicker()
        when {
            !core.ble.isBluetoothOn -> toast("Bluetooth is off — the demo device is still available")
            hasPermissions() -> startBleScan()
            else -> permissionLauncher.launch(requiredPermissions())
        }
    }

    private fun startBleScan() {
        if (deviceDialog?.isShowing != true) return
        core.foundDevices.clear()
        core.ble.pollIntervalMs = Prefs.pollMs(this)
        core.ble.startScan()
    }

    private var deviceDialog: AlertDialog? = null
    private lateinit var deviceListAdapter: ArrayAdapter<String>

    /** Fixed rows before the scanned devices: demo, then optional reconnect. */
    private var pickerLastDevice: Pair<String, String>? = null

    @SuppressLint("MissingPermission")
    private fun showDevicePicker() {
        core.foundDevices.clear()
        pickerLastDevice = Prefs.lastDevice(this)
        deviceListAdapter = ArrayAdapter(this, android.R.layout.simple_list_item_1)
        deviceListAdapter.add(getString(R.string.demo_device))
        pickerLastDevice?.let { (addr, name) ->
            deviceListAdapter.add(getString(R.string.reconnect_row, name, addr))
        }
        val fixedRows = 1 + (if (pickerLastDevice != null) 1 else 0)
        deviceDialog = MaterialAlertDialogBuilder(this)
            .setTitle("Select device")
            .setAdapter(deviceListAdapter) { _, which ->
                when {
                    which == 0 -> showDemoConfigDialog(applyLive = false) { startSimulator() }
                    which == 1 && pickerLastDevice != null -> {
                        val (addr, name) = pickerLastDevice!!
                        when {
                            !hasPermissions() -> {
                                toast("Grant the Bluetooth permission, then scan to reconnect")
                                permissionLauncher.launch(requiredPermissions())
                            }
                            else -> {
                                // Scan-based reconnect (reconnect() stops the picker
                                // scan itself), so a random-address device reconnects.
                                if (!core.ble.reconnect(addr)) toast("Could not reconnect to $name — is Bluetooth on?")
                            }
                        }
                    }
                    else -> core.foundDevices.values.toList().getOrNull(which - fixedRows)?.let { dev ->
                        Prefs.setLastDevice(this, dev.address,
                            try { dev.name ?: "EL15" } catch (se: SecurityException) { "EL15" })
                        core.ble.connect(dev)
                    }
                }
            }
            .setNegativeButton("Cancel") { _, _ -> core.ble.stopScan() }
            .setOnDismissListener { core.ble.stopScan() }.show()
    }

    // ---- Demo device ------------------------------------------------------
    private fun startSimulator() {
        core.ble.stopScan(); deviceDialog?.dismiss()
        maxVoltageSeen = 0f
        core.startSimulator()
        clearReadouts()
        renderConnectionState()
    }

    private fun updateDemoStatusText() {
        binding.monitor.connStatusText.text = "Demo simulator"
        binding.monitor.connStatusSub.text =
            "%.1f V · %.3f Ω · tap to edit circuit".format(core.demoEmf, core.demoSeriesR)
    }

    private fun showDemoConfigDialog(applyLive: Boolean, onDone: (() -> Unit)?) {
        val view = layoutInflater.inflate(R.layout.dialog_demo_circuit, null)
        val emfIn = view.findViewById<TextInputEditText>(R.id.demoEmfInput)
        val resIn = view.findViewById<TextInputEditText>(R.id.demoResInput)
        emfIn.setText(fmtField(core.demoEmf)); resIn.setText(fmtField(core.demoSeriesR))
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.demo_cfg_title).setView(view)
            .setPositiveButton(if (onDone != null) R.string.demo_cfg_connect else R.string.demo_cfg_apply) { _, _ ->
                val emf = (emfIn.text.toString().trim().toFloatOrNull() ?: core.demoEmf).coerceIn(0.1f, 100f)
                val r = (resIn.text.toString().trim().toFloatOrNull() ?: core.demoSeriesR).coerceIn(0f, 100f)
                Prefs.setDemo(this, emf, r)
                core.applyDemoCircuit(emf, r)
                if (applyLive && core.isDemo) updateDemoStatusText()
                onDone?.invoke()
            }
            .setNegativeButton(R.string.cancel, null).show()
    }

    private fun fmtField(v: Float) = if (v == v.toLong().toFloat()) v.toLong().toString() else v.toString()

    // ---- Waveform CSV -----------------------------------------------------
    private fun buildWaveformCsv(rows: List<WaveformView.WPoint>): ByteArray {
        val sb = StringBuilder("time_ms,elapsed_s,voltage_V,current_A,power_W,temp_C,fan\n")
        val t0 = rows.first().tMs
        for (r in rows) sb.append("%d,%.3f,%.4f,%.4f,%.4f,%.2f,%d\n".format(
            Locale.US, r.tMs, (r.tMs - t0) / 1000.0, r.v, r.i, r.p, r.temp, r.fan))
        return sb.toString().toByteArray()
    }

    private fun waveformExportChooser() {
        // Prefer the core's recorded log (survives app backgrounding); fall
        // back to whatever the on-screen waveform holds.
        val rows = if (core.recordLog.isNotEmpty()) core.recordLog.toList()
        else binding.monitor.waveformView.exportRows()
        if (rows.isEmpty()) { toast("No waveform data yet"); return }
        val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US)
            .format(java.util.Date())
        val name = "el15-waveform-$stamp.csv"
        MaterialAlertDialogBuilder(this)
            .setTitle(getString(R.string.wave_export_title, rows.size))
            .setItems(arrayOf(getString(R.string.rt_share), getString(R.string.rt_save_device))) { _, which ->
                try {
                    val bytes = buildWaveformCsv(rows)
                    if (which == 0) {
                        Exporter.share(this, name, "text/csv", bytes, "EL15 waveform (${rows.size} samples)")
                    } else {
                        withStoragePermission {
                            val where = Exporter.saveToDownloads(this, name, "text/csv", bytes)
                            toast(if (where != null) getString(R.string.rt_saved_to, where) else "Save failed")
                        }
                    }
                } catch (e: Exception) { toast("Export failed: ${e.message}") }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private var pendingStorageAction: (() -> Unit)? = null
    private val storagePermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) pendingStorageAction?.invoke() else toast("Storage permission denied")
        pendingStorageAction = null
    }

    private fun withStoragePermission(action: () -> Unit) {
        if (!Exporter.needsLegacyWritePermission() ||
            ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
            PackageManager.PERMISSION_GRANTED
        ) {
            action()
        } else {
            pendingStorageAction = action
            storagePermission.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        }
    }

    // ---- Status / rendering ----------------------------------------------
    private fun setConnDot(connected: Boolean) {
        binding.monitor.connDot.backgroundTintList =
            ColorStateList.valueOf(color(if (connected) R.color.value_green else R.color.value_red))
    }

    private fun clearReadouts() = with(binding.monitor) {
        loadToggle.setText(R.string.load_on)
        loadToggle.backgroundTintList = ColorStateList.valueOf(color(R.color.value_green))
        lockButton.setText(R.string.lock)
        gaugeVoltage.set(0f, 20f, 2, "VOLTS · 0–20V", false)
        gaugeCurrent.set(0f, 12f, 3, "AMPS · 0–12A", false)
        powerValue.text = "—"; powerBar.progress = 0; modeValue.text = "—"; extraValue.text = ""
        warningBox.visibility = View.GONE
        packetHex.text = "— no packets —"; packetCrc.text = "CRC —"
        packetCrc.setTextColor(color(R.color.muted))
        waveStats.text = "—"; waveformView.clear()
    }

    /** Renders connection status, button labels, and control enablement. */
    private fun renderConnectionState() = with(binding.monitor) {
        val state = core.ble.state
        when {
            core.isDemo -> updateDemoStatusText()
            state == El15BleManager.State.CONNECTED -> {
                connStatusText.text = "EL15"; connStatusSub.text = "Connected · FFF0"
            }
            state == El15BleManager.State.CONNECTING -> {
                connStatusText.text = "Connecting…"; connStatusSub.text = ""
            }
            state == El15BleManager.State.SCANNING -> {
                connStatusText.text = "Scanning…"; connStatusSub.text = ""
            }
            else -> {
                connStatusText.text = getString(R.string.disconnected)
                connStatusSub.text = getString(R.string.no_device)
            }
        }
        connButton.text = when {
            core.isDemo -> getString(R.string.disconnect)
            state == El15BleManager.State.CONNECTED -> getString(R.string.disconnect)
            state == El15BleManager.State.CONNECTING -> getString(R.string.cancel)
            else -> getString(R.string.scan_connect)
        }
        setConnDot(core.isConnected)
        updateControlsEnabled()
    }

    // ---- DeviceCore.Ui: transport ---------------------------------------------
    override fun coreStateChanged() {
        val connected = core.isConnected
        renderConnectionState()
        // A test may have been stopped from outside this UI (notification,
        // widget, QS tile) — resync the Start/Stop controls with the engines.
        with(binding.monitor) {
            if (!core.tester.running && !core.calibrator.running) {
                startTestButton.setText(R.string.rt_start)
                testBar.visibility = View.GONE
                if (core.lastProgressText.isNotEmpty() && testProgressText.visibility == View.VISIBLE) {
                    testProgressText.text = core.lastProgressText
                }
            }
            if (!core.session.running) {
                benchStopButton.visibility = View.GONE
                if (core.lastProgressText.isNotEmpty() && benchProgressText.visibility == View.VISIBLE) {
                    benchProgressText.text = core.lastProgressText
                }
            }
        }
        if (!connected) {
            onTestFinishedUi(null)
            onBenchFinishedUi(null)
            maxVoltageSeen = 0f
            clearReadouts()
            if (pendingThemeRecreate && !core.busy) {
                pendingThemeRecreate = false
                recreate()
                return
            }
        }
        if (core.ble.state != El15BleManager.State.SCANNING && !core.isDemo &&
            core.ble.state != El15BleManager.State.IDLE) {
            deviceDialog?.dismiss()
        }
    }

    override fun coreDeviceFound(address: String, name: String) {
        if (deviceDialog?.isShowing == true) {
            deviceListAdapter.add("$name\n$address")
            deviceListAdapter.notifyDataSetChanged()
        }
    }

    override fun coreStatus(status: El15Status) = renderStatus(status)

    private fun renderStatus(status: El15Status): Unit = with(binding.monitor) {
        // Packet inspector shows every frame, including corrupt ones…
        packetHex.text = status.raw.ifEmpty { "— no packets —" }
        packetCrc.text = "CRC ${if (status.crcPass) "✓" else "✗"}"
        packetCrc.setTextColor(color(if (status.crcPass) R.color.value_green else R.color.value_red))
        // …but nothing else may consume a corrupt/truncated frame's values.
        if (!status.valid) return

        maxVoltageSeen = maxOf(maxVoltageSeen, status.voltage)
        val vMax = maxOf(15f, ceil((maxVoltageSeen + 3f) / 5f) * 5f)
        gaugeVoltage.set(status.voltage, vMax, 2, "VOLTS · 0–${vMax.toInt()}V", true)
        val curr = if (status.mode == El15Protocol.MODE_DCR) status.dcrI1 else status.current
        gaugeCurrent.set(curr, 12f, 3, "AMPS · 0–12A", true)

        powerValue.text = if (status.mode == El15Protocol.MODE_DCR) "—" else "%.1f W".format(status.power)
        powerBar.progress = status.power.toInt().coerceIn(0, 150)
        modeValue.text = status.modeName
        extraValue.text = buildExtraLine(status)

        loadToggle.text = if (status.loadOn) getString(R.string.load_off) else getString(R.string.load_on)
        loadToggle.backgroundTintList = ColorStateList.valueOf(color(if (status.loadOn) R.color.value_red else R.color.value_green))
        lockButton.setText(if (status.lockOn) R.string.locked else R.string.lock)

        if (status.warning.isNotEmpty()) {
            warningBox.visibility = View.VISIBLE
            warningText.text = getString(R.string.protection, status.warning)
        } else warningBox.visibility = View.GONE

        if (panel == Panel.DEVICE) {
            val chipId = chipToMode.entries.firstOrNull { it.value == status.mode }?.key
            updatingChips = true
            if (chipId != null) modeChipGroup.check(chipId) else modeChipGroup.clearCheck()
            updatingChips = false
        }

        setpointLayout.hint = "${status.setpointLabel} (${status.setpointUnit})"
        if (!userEditingSetpoint && status.setpointInPacket) {
            // Locale.US: this text is re-parsed by toFloatOrNull on SET, which
            // rejects comma decimals.
            setpointInput.setText("%.${status.setpointDecimals}f".format(Locale.US, status.setpoint))
        }

        waveformView.add(status.voltage, curr, status.power, status.temperature, status.fanSpeed, System.currentTimeMillis())
        waveStats.text = waveformView.statsText()
    }

    private fun buildExtraLine(s: El15Status): String {
        val parts = mutableListOf<String>()
        when (s.mode) {
            El15Protocol.MODE_CAP -> { parts += "Energy %.3f Wh".format(s.energyWh); parts += "Capacity %.3f Ah".format(s.capacityAh) }
            El15Protocol.MODE_DCR -> { parts += "R %.1f mΩ".format(s.dcrMilliOhm); parts += "I2 %.3f A".format(s.dcrI2) }
            else -> {
                if (s.runtime > 0) parts += "Runtime ${formatRuntime(s.runtime)}"
                if (s.temperature != 0f) parts += "Temp %.1f°C".format(s.temperature)
            }
        }
        parts += "Fan ${s.fanSpeed}/${El15Protocol.FAN_SPEED_MAX}"
        parts += if (s.ready) "Ready" else "Idle"
        return parts.joinToString("   ")
    }

    private fun formatRuntime(seconds: Int): String {
        val h = seconds / 3600; val m = (seconds % 3600) / 60; val sec = seconds % 60
        return if (h > 0) "%d:%02d:%02d".format(h, m, sec) else "%02d:%02d".format(m, sec)
    }

    private fun updateControlsEnabled() = with(binding.monitor) {
        val connected = core.isConnected
        val busy = core.busy
        val idle = connected && !busy
        val deviceMode = idle && panel == Panel.DEVICE
        for (id in chipToMode.keys) findViewById<Chip>(id).isEnabled = idle
        chipRtest.isEnabled = idle
        chipBench.isEnabled = idle
        setpointInput.isEnabled = deviceMode; setSetpointButton.isEnabled = deviceMode
        lockButton.isEnabled = deviceMode
        // Enabled while disconnected on purpose: tapping it opens the scan
        // dialog (see the click handler). Only a running sweep locks it.
        loadToggle.isEnabled = !busy
        fuseInput.isEnabled = idle; stepsInput.isEnabled = idle; settleInput.isEnabled = idle; sampleInput.isEnabled = idle
        startTestButton.isEnabled = connected && !core.calibrator.running && !core.session.running
        calibrateButton.isEnabled = connected && !core.tester.running && !core.session.running
        calibrateButton.setText(if (core.calibrator.running) R.string.rt_cal_stop else R.string.rt_calibrate)
        benchCapacity.isEnabled = idle
        benchRuntime.isEnabled = idle
        benchStep.isEnabled = idle
        benchOcp.isEnabled = idle
        benchStopButton.visibility = if (core.session.running) View.VISIBLE else View.GONE
    }

    private fun toast(msg: String) = android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_SHORT).show()
}
