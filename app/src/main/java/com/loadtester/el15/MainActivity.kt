package com.loadtester.el15

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
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
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.core.content.ContextCompat
import com.google.android.material.chip.Chip
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.TextInputEditText
import com.loadtester.el15.databinding.ActivityMainBinding
import kotlin.math.ceil

class MainActivity : BaseActivity(), El15BleManager.Listener, CircuitResistanceTester.Callback {

    private lateinit var binding: ActivityMainBinding
    private lateinit var ble: El15BleManager
    private lateinit var tester: CircuitResistanceTester
    private lateinit var calibrator: SweepCalibrator

    private val foundDevices = LinkedHashMap<String, BluetoothDevice>()
    private var lastStatus: El15Status? = null
    private var userEditingSetpoint = false
    private var updatingChips = false
    private var rtestActive = false
    private var maxVoltageSeen = 0f

    private var simulator: El15Simulator? = null
    private var pendingThemeRecreate = false
    private var nightMode = -1
    private var pendingResultId: String? = null
    private var demoEmf = 12.6f
    private var demoSeriesR = 0.35f

    private val controller: El15Controller get() = simulator ?: ble
    private val activeController = object : El15Controller {
        override fun setMode(mode: Int) = controller.setMode(mode)
        override fun setSetpoint(value: Float) = controller.setSetpoint(value)
        override fun setLoad(on: Boolean) = controller.setLoad(on)
        override fun setLock() = controller.setLock()
    }

    private fun isConnected(): Boolean =
        simulator != null || ble.state == El15BleManager.State.CONNECTED

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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        ble = El15BleManager(this).also { it.listener = this }
        tester = CircuitResistanceTester(activeController, this)
        calibrator = SweepCalibrator(activeController, calCallback)
        nightMode = resources.configuration.uiMode and
            android.content.res.Configuration.UI_MODE_NIGHT_MASK
        demoEmf = Prefs.demoEmf(this)
        demoSeriesR = Prefs.demoR(this)

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
        applyLegend()
        clearReadouts()
        updateControlsEnabled(false)
    }

    override fun onResume() {
        super.onResume()
        pendingResultId?.let { id ->
            pendingResultId = null
            startActivity(Intent(this, ResultActivity::class.java)
                .putExtra(ResultActivity.EXTRA_RECORD_ID, id))
        }
        val poll = Prefs.pollMs(this)
        ble.pollIntervalMs = poll; simulator?.pollIntervalMs = poll
        tester.pollIntervalMs = poll
        tester.safetyFactor = Prefs.safetyFactor(this)
        calibrator.pollIntervalMs = poll
        calibrator.safetyFactor = Prefs.safetyFactor(this)
        // Demo circuit may have been edited in Settings; apply it live.
        val newEmf = Prefs.demoEmf(this); val newR = Prefs.demoR(this)
        if (newEmf != demoEmf || newR != demoSeriesR) {
            demoEmf = newEmf; demoSeriesR = newR
            simulator?.let { it.emf = demoEmf; it.seriesR = demoSeriesR; updateDemoStatusText() }
        }
        if (Prefs.keepScreenOn(this)) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        // configChanges covers several kinds; only a real day/night flip should
        // trigger a re-theme. (uiMode is handled so a flip cannot destroy a
        // live BLE session or abort a running sweep.)
        val newNight = newConfig.uiMode and android.content.res.Configuration.UI_MODE_NIGHT_MASK
        if (newNight == nightMode) return
        nightMode = newNight
        if (!isConnected() && !tester.running) {
            recreate()
        } else {
            pendingThemeRecreate = true
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        val wasTesting = tester.running || calibrator.running
        if (calibrator.running) calibrator.stop()
        if (tester.running) tester.stop()
        simulator?.stop()
        // shutdownAndDisconnect writes LOAD_OFF directly on the GATT before
        // teardown — the queued writes from tester.stop() would otherwise be
        // cleared by a plain disconnect and the load left sinking current.
        if (wasTesting) ble.shutdownAndDisconnect() else ble.disconnect()
    }

    private fun color(res: Int) = ContextCompat.getColor(this, res)

    private fun showAbout() = MaterialAlertDialogBuilder(this)
        .setTitle(getString(R.string.app_name))
        .setMessage(getString(R.string.about_body, BuildConfig.VERSION_NAME))
        .setPositiveButton(android.R.string.ok, null).show()

    // ---- Controls ---------------------------------------------------------
    private fun setupControls() = with(binding.monitor) {
        connButton.setOnClickListener {
            if (isConnected() || ble.state == El15BleManager.State.CONNECTING) disconnectAll() else requestScan()
        }
        connStatusTap.setOnClickListener {
            if (simulator != null) showDemoConfigDialog(applyLive = true, onDone = null)
        }
        setpointInput.setOnFocusChangeListener { _, f -> userEditingSetpoint = f }
        setSetpointButton.setOnClickListener {
            val v = setpointInput.text.toString().trim().toFloatOrNull()
            if (v == null) toast("Enter a valid number")
            else { controller.setSetpoint(v); setpointInput.clearFocus(); toast("Setpoint → $v") }
        }
        loadToggle.setOnClickListener {
            if (!isConnected()) requestScan() else controller.setLoad(!(lastStatus?.loadOn ?: false))
        }
        lockButton.setOnClickListener { controller.setLock() }
    }

    private fun setupModeChips() = with(binding.monitor) {
        for (id in chipToMode.keys) findViewById<Chip>(id).setOnClickListener {
            if (updatingChips) return@setOnClickListener
            rtestActive = false
            deviceControls.visibility = View.VISIBLE
            rtestPanel.visibility = View.GONE
            chipToMode[id]?.let { controller.setMode(it) }
            updateControlsEnabled(isConnected())
        }
        chipRtest.setOnClickListener {
            if (updatingChips) return@setOnClickListener
            if (rtestActive && !chipRtest.isChecked) {
                // Re-tap on the active chip: keep it selected, panel stays.
                updatingChips = true; chipRtest.isChecked = true; updatingChips = false
                return@setOnClickListener
            }
            rtestActive = true
            deviceControls.visibility = View.GONE
            rtestPanel.visibility = View.VISIBLE
            if (isConnected()) controller.setLoad(false) // clear the load before a sweep
            updateControlsEnabled(isConnected())
        }
    }

    private fun setupWaveformControls() = with(binding.monitor) {
        wavePauseButton.setOnClickListener {
            waveformView.paused = !waveformView.paused
            wavePauseButton.setIconResource(if (waveformView.paused) R.drawable.ic_play else R.drawable.ic_pause)
        }
        waveRecordButton.setOnClickListener {
            if (waveformView.recording) { waveformView.stopRecording(); toast("Recording stopped (${waveformView.recordedCount()} samples)") }
            else { waveformView.startRecording(); toast("Recording…") }
        }
        waveExportButton.setOnClickListener { waveformExportChooser() }
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

    // ---- Resistance test --------------------------------------------------
    private fun setupTest() = with(binding.monitor) {
        if (stepsInput.text.isNullOrBlank()) stepsInput.setText(Prefs.steps(this@MainActivity).toString())
        if (settleInput.text.isNullOrBlank()) settleInput.setText(Prefs.settleMs(this@MainActivity).toString())
        if (sampleInput.text.isNullOrBlank()) sampleInput.setText(Prefs.sampleMs(this@MainActivity).toString())

        fun info(layout: com.google.android.material.textfield.TextInputLayout, title: Int, body: Int) {
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
            if (calibrator.running) {
                calibrator.stop()
                onTestFinishedUi("Calibration stopped")
                return@setOnClickListener
            }
            if (!isConnected()) { toast("Connect to the EL15 (or the demo) first"); return@setOnClickListener }
            val fuse = fuseInput.text.toString().trim().toFloatOrNull()
            if (fuse == null || fuse <= 0f) { toast("Enter the circuit's fuse rating in amps"); return@setOnClickListener }
            if (fuse > 200f) { toast("That fuse rating looks too high — double-check it"); return@setOnClickListener }
            val (peak, _) = predictedPeak(fuse)
            val poll = Prefs.pollMs(this@MainActivity)
            // Probes (~6s+6s) + priming + three short sweeps at default-ish timing.
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
            if (tester.running) { tester.stop(); onTestFinishedUi("Test stopped"); return@setOnClickListener }
            if (!isConnected()) { toast("Connect to the EL15 (or the demo) first"); return@setOnClickListener }
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
        tester.steps = steps; tester.settleMs = settle; tester.collectMs = sample
        stepsInput.setText(steps.toString()); settleInput.setText(settle.toString()); sampleInput.setText(sample.toString())
        true
    }

    private fun predictedPeak(fuse: Float): Pair<Float, String> {
        val v = lastStatus?.voltage?.takeIf { it > El15Protocol.MIN_VOLTAGE_V } ?: El15Protocol.MAX_VOLTAGE_V
        val fuseCap = fuse * tester.safetyFactor
        val powerCap = El15Protocol.MAX_POWER_W / v
        val peak = minOf(fuseCap, powerCap, El15Protocol.MAX_CURRENT_A)
        val limiter = when (peak) {
            fuseCap -> "%.0f%% of the %.1f A fuse".format(tester.safetyFactor * 100, fuse)
            powerCap -> "the %.0f W power limit at %.1f V".format(El15Protocol.MAX_POWER_W, v)
            else -> "the %.0f A current limit".format(El15Protocol.MAX_CURRENT_A)
        }
        return peak to limiter
    }

    private fun startTest(fuse: Float) = with(binding.monitor) {
        fuseInput.clearFocus()
        testBar.visibility = View.VISIBLE; testBar.progress = 0
        testProgressText.visibility = View.VISIBLE
        // Estimate with the *effective* windows (the engine stretches short
        // ones to the poll rate), formatted as minutes for long sweeps.
        val poll = Prefs.pollMs(this@MainActivity)
        val perStep = maxOf(tester.settleMs, poll + 100) + maxOf(tester.collectMs, 2 * poll + 200)
        val estS = tester.steps.toLong() * perStep / 1000L
        val estStr = if (estS >= 120) "~${estS / 60}m ${estS % 60}s" else "~${estS}s"
        testProgressText.text = "Priming… ($estStr)"
        startTestButton.setText(R.string.rt_stop)
        tester.start(fuse) // start FIRST so tester.running locks the controls below
        updateControlsEnabled(true)
    }

    private fun onTestFinishedUi(message: String?) = with(binding.monitor) {
        startTestButton.setText(R.string.rt_start)
        testBar.visibility = View.GONE
        message?.let { testProgressText.text = it }
        updateControlsEnabled(isConnected())
    }

    override fun onTestProgress(step: Int, totalSteps: Int, targetCurrent: Float, voltage: Float, current: Float) = with(binding.monitor) {
        testBar.progress = (step * 100 / totalSteps).coerceIn(0, 100)
        testProgressText.text = "Step %d/%d  →  %.3f A set   %.3f V @ %.3f A".format(step, totalSteps, targetCurrent, voltage, current)
    }

    override fun onTestComplete(result: CircuitResistanceTester.ResistanceResult) {
        val device = simulator?.let { "Demo (%.1f V, %.3f Ω)".format(demoEmf, demoSeriesR) } ?: "EL15 (BLE)"
        val record = TestRecord.from(
            result, tester.steps, tester.settleMs, tester.collectMs,
            (tester.safetyFactor * 100).toInt(), device, System.currentTimeMillis(),
        )
        if (!TestRepository(this).save(record)) {
            onTestFinishedUi("Done — R = ${formatOhm(result.resistanceOhm)}")
            toast("Could not archive the test result")
            return
        }
        if (lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED)) {
            onTestFinishedUi("Done — R = ${formatOhm(result.resistanceOhm)}")
            startActivity(Intent(this, ResultActivity::class.java)
                .putExtra(ResultActivity.EXTRA_RECORD_ID, record.id))
        } else {
            // Background activity starts are blocked; open it when we return.
            onTestFinishedUi("Done — R = ${formatOhm(result.resistanceOhm)} (saved to History)")
            pendingResultId = record.id
        }
    }

    override fun onTestError(message: String) {
        onTestFinishedUi(null)
        binding.monitor.testProgressText.text = message
        MaterialAlertDialogBuilder(this).setTitle("Test stopped").setMessage(message)
            .setPositiveButton(android.R.string.ok, null).show()
    }

    private fun startCalibration(fuse: Float) = with(binding.monitor) {
        fuseInput.clearFocus()
        testBar.visibility = View.GONE
        testProgressText.visibility = View.VISIBLE
        testProgressText.text = "Calibrating…"
        calibrator.start(fuse)
        updateControlsEnabled(true)
    }

    private val calCallback = object : SweepCalibrator.Callback {
        override fun onCalProgress(message: String) {
            binding.monitor.testProgressText.text = message
        }

        override fun onCalComplete(result: SweepCalibrator.CalResult) {
            onTestFinishedUi("Calibration done — recommended ${result.steps} steps")
            val sweeps = result.sweepResistances.joinToString(" → ") { formatOhm(it) }
            val body = StringBuilder()
                .append("Steps: ${result.steps}")
                .append(if (result.converged) "  (resistance converged: $sweeps)" else "  (did not fully converge: $sweeps — using extra headroom)")
                .append("\n\nSettle: ${result.settleMs} ms  (measured stabilization)")
                .append("\nSample: ${result.sampleMs} ms  (noise σ %.1f mV)".format(result.noiseVolts * 1000f))
                .append("\n\nEstimated test duration: ~${result.estimatedTestS}s")
                .toString()
            MaterialAlertDialogBuilder(this@MainActivity)
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

        override fun onCalError(message: String) {
            onTestFinishedUi(null)
            binding.monitor.testProgressText.text = message
            MaterialAlertDialogBuilder(this@MainActivity)
                .setTitle("Calibration stopped").setMessage(message)
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
            !ble.isBluetoothOn -> toast("Bluetooth is off — the demo device is still available")
            hasPermissions() -> startBleScan()
            else -> permissionLauncher.launch(requiredPermissions())
        }
    }

    private fun startBleScan() {
        if (deviceDialog?.isShowing != true) return
        foundDevices.clear(); ble.pollIntervalMs = Prefs.pollMs(this); ble.startScan()
    }

    private var deviceDialog: AlertDialog? = null
    private lateinit var deviceListAdapter: ArrayAdapter<String>

    @SuppressLint("MissingPermission")
    private fun showDevicePicker() {
        foundDevices.clear()
        deviceListAdapter = ArrayAdapter(this, android.R.layout.simple_list_item_1)
        deviceListAdapter.add(getString(R.string.demo_device))
        deviceDialog = MaterialAlertDialogBuilder(this)
            .setTitle("Select device")
            .setAdapter(deviceListAdapter) { _, which ->
                if (which == 0) showDemoConfigDialog(applyLive = false) { startSimulator() }
                else foundDevices.values.toList().getOrNull(which - 1)?.let { ble.connect(it) }
            }
            .setNegativeButton("Cancel") { _, _ -> ble.stopScan() }
            .setOnDismissListener { ble.stopScan() }.show()
    }

    // ---- Demo device ------------------------------------------------------
    private fun startSimulator() {
        ble.stopScan(); deviceDialog?.dismiss()
        val sim = El15Simulator({ s -> runOnUiThread { handleIncomingStatus(s) } }, demoEmf, demoSeriesR)
        sim.pollIntervalMs = Prefs.pollMs(this)
        simulator = sim; sim.start()
        maxVoltageSeen = 0f
        updateDemoStatusText()
        setConnDot(true)
        binding.monitor.connButton.text = getString(R.string.disconnect)
        clearReadouts(); updateControlsEnabled(true)
    }

    private fun stopSimulator() {
        if (tester.running) tester.stop()
        simulator?.stop(); simulator = null
        if (pendingThemeRecreate) { pendingThemeRecreate = false; recreate(); return }
        binding.monitor.connStatusText.text = getString(R.string.disconnected)
        binding.monitor.connStatusSub.text = getString(R.string.no_device)
        binding.monitor.connButton.text = getString(R.string.scan_connect)
        setConnDot(false); updateControlsEnabled(false); clearReadouts()
    }

    private fun updateDemoStatusText() {
        binding.monitor.connStatusText.text = "Demo simulator"
        binding.monitor.connStatusSub.text = "%.1f V · %.3f Ω · tap to edit circuit".format(demoEmf, demoSeriesR)
    }

    private fun disconnectAll() {
        if (simulator != null) {
            stopSimulator()
            return
        }
        // Stop the tester/calibrator BEFORE tearing BLE down, and use the
        // safe shutdown path so the final LOAD_OFF is written even mid-sweep.
        val wasTesting = tester.running || calibrator.running
        if (calibrator.running) { calibrator.stop(); onTestFinishedUi("Calibration stopped") }
        if (tester.running) {
            tester.stop()
            onTestFinishedUi("Test stopped")
        }
        if (wasTesting) ble.shutdownAndDisconnect() else ble.disconnect()
    }

    private fun showDemoConfigDialog(applyLive: Boolean, onDone: (() -> Unit)?) {
        val view = layoutInflater.inflate(R.layout.dialog_demo_circuit, null)
        val emfIn = view.findViewById<TextInputEditText>(R.id.demoEmfInput)
        val resIn = view.findViewById<TextInputEditText>(R.id.demoResInput)
        emfIn.setText(fmtField(demoEmf)); resIn.setText(fmtField(demoSeriesR))
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.demo_cfg_title).setView(view)
            .setPositiveButton(if (onDone != null) R.string.demo_cfg_connect else R.string.demo_cfg_apply) { _, _ ->
                demoEmf = (emfIn.text.toString().trim().toFloatOrNull() ?: demoEmf).coerceIn(0.1f, 100f)
                demoSeriesR = (resIn.text.toString().trim().toFloatOrNull() ?: demoSeriesR).coerceIn(0f, 100f)
                Prefs.setDemo(this, demoEmf, demoSeriesR)
                if (applyLive) { simulator?.let { it.emf = demoEmf; it.seriesR = demoSeriesR }; updateDemoStatusText() }
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
            java.util.Locale.US, r.tMs, (r.tMs - t0) / 1000.0, r.v, r.i, r.p, r.temp, r.fan))
        return sb.toString().toByteArray()
    }

    private fun waveformExportChooser() {
        val rows = binding.monitor.waveformView.exportRows()
        if (rows.isEmpty()) { toast("No waveform data yet"); return }
        val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
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
        loadToggle.backgroundTintList =
            android.content.res.ColorStateList.valueOf(color(R.color.value_green))
        lockButton.setText(R.string.lock)
        gaugeVoltage.set(0f, 20f, 2, "VOLTS · 0–20V", false)
        gaugeCurrent.set(0f, 12f, 3, "AMPS · 0–12A", false)
        powerValue.text = "—"; powerBar.progress = 0; modeValue.text = "—"; extraValue.text = ""
        warningBox.visibility = View.GONE
        packetHex.text = "— no packets —"; packetCrc.text = "CRC —"
        packetCrc.setTextColor(color(R.color.muted))
        waveStats.text = "—"; waveformView.clear()
    }

    override fun onStateChanged(state: El15BleManager.State, info: String) {
        if (simulator != null) return
        val connected = state == El15BleManager.State.CONNECTED
        if (!connected && tester.running) { tester.stop(); onTestFinishedUi("Test stopped (disconnected)") }
        if (!connected && calibrator.running) { calibrator.stop(); onTestFinishedUi("Calibration stopped (disconnected)") }
        with(binding.monitor) {
            when (state) {
                El15BleManager.State.CONNECTED -> { connStatusText.text = "EL15"; connStatusSub.text = "Connected · FFF0" }
                El15BleManager.State.CONNECTING -> { connStatusText.text = info; connStatusSub.text = "" }
                El15BleManager.State.SCANNING -> { connStatusText.text = info; connStatusSub.text = "" }
                else -> { connStatusText.text = getString(R.string.disconnected); connStatusSub.text = getString(R.string.no_device) }
            }
            connButton.text = when (state) {
                El15BleManager.State.CONNECTED -> getString(R.string.disconnect)
                El15BleManager.State.CONNECTING -> getString(R.string.cancel)
                else -> getString(R.string.scan_connect)
            }
        }
        setConnDot(connected)
        updateControlsEnabled(connected)
        if (state != El15BleManager.State.SCANNING) deviceDialog?.dismiss()
        if (!connected) { maxVoltageSeen = 0f; clearReadouts() }
        if (!connected && pendingThemeRecreate && !tester.running) {
            pendingThemeRecreate = false
            recreate()
        }
    }

    @SuppressLint("MissingPermission")
    override fun onDeviceFound(device: BluetoothDevice, name: String) {
        if (foundDevices.put(device.address, device) == null) {
            deviceListAdapter.add("$name\n${device.address}")
            deviceListAdapter.notifyDataSetChanged()
        }
    }

    override fun onStatus(status: El15Status) = handleIncomingStatus(status)

    private fun handleIncomingStatus(status: El15Status): Unit = with(binding.monitor) {
        // Packet inspector shows every frame, including corrupt ones…
        packetHex.text = status.raw.ifEmpty { "— no packets —" }
        packetCrc.text = "CRC ${if (status.crcPass) "✓" else "✗"}"
        packetCrc.setTextColor(color(if (status.crcPass) R.color.value_green else R.color.value_red))
        // …but nothing else may consume a corrupt/truncated frame's values.
        if (!status.valid) return
        lastStatus = status
        if (tester.running) tester.onStatus(status)
        if (calibrator.running) calibrator.onStatus(status)

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

        if (!rtestActive) {
            val chipId = chipToMode.entries.firstOrNull { it.value == status.mode }?.key
            updatingChips = true
            if (chipId != null) modeChipGroup.check(chipId) else modeChipGroup.clearCheck()
            updatingChips = false
        }

        setpointLayout.hint = "${status.setpointLabel} (${status.setpointUnit})"
        if (!userEditingSetpoint && status.setpointInPacket) {
            // Locale.US: this text is re-parsed by toFloatOrNull on SET, which
            // rejects comma decimals.
            setpointInput.setText("%.${status.setpointDecimals}f".format(java.util.Locale.US, status.setpoint))
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

    override fun onLog(message: String) {}

    private fun updateControlsEnabled(connected: Boolean) = with(binding.monitor) {
        val busy = tester.running || calibrator.running
        val idle = connected && !busy
        val deviceMode = idle && !rtestActive
        for (id in chipToMode.keys) findViewById<Chip>(id).isEnabled = idle
        chipRtest.isEnabled = idle
        setpointInput.isEnabled = deviceMode; setSetpointButton.isEnabled = deviceMode
        lockButton.isEnabled = deviceMode
        // Enabled while disconnected on purpose: tapping it opens the scan
        // dialog (see the click handler). Only a running sweep locks it.
        loadToggle.isEnabled = !busy
        fuseInput.isEnabled = idle; stepsInput.isEnabled = idle; settleInput.isEnabled = idle; sampleInput.isEnabled = idle
        startTestButton.isEnabled = connected && !calibrator.running
        calibrateButton.isEnabled = connected && !tester.running
        calibrateButton.setText(if (calibrator.running) R.string.rt_cal_stop else R.string.rt_calibrate)
    }

    private fun toast(msg: String) = android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_SHORT).show()
}
