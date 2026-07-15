package com.loadtester.el15

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.net.Uri
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
import androidx.core.content.FileProvider
import com.google.android.material.chip.Chip
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.TextInputEditText
import com.loadtester.el15.databinding.ActivityMainBinding
import java.io.File
import java.io.FileOutputStream
import kotlin.math.ceil

class MainActivity : BaseActivity(), El15BleManager.Listener, CircuitResistanceTester.Callback {

    private lateinit var binding: ActivityMainBinding
    private lateinit var ble: El15BleManager
    private lateinit var tester: CircuitResistanceTester

    private val foundDevices = LinkedHashMap<String, BluetoothDevice>()
    private var lastStatus: El15Status? = null
    private var userEditingSetpoint = false
    private var updatingChips = false
    private var rtestActive = false
    private var maxVoltageSeen = 0f

    private var simulator: El15Simulator? = null
    private var pendingThemeRecreate = false
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
        demoEmf = Prefs.demoEmf(this)
        demoSeriesR = Prefs.demoR(this)

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
        val poll = Prefs.pollMs(this)
        ble.pollIntervalMs = poll; simulator?.pollIntervalMs = poll
        tester.pollIntervalMs = poll
        tester.safetyFactor = Prefs.safetyFactor(this)
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
        // uiMode is in configChanges so a day/night flip cannot destroy a live
        // BLE session or abort a running sweep. Restyle by recreating, but only
        // when idle; otherwise keep the stale palette until the session ends.
        if (!isConnected() && !tester.running) {
            recreate()
        } else {
            pendingThemeRecreate = true
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        val wasTesting = tester.running
        if (wasTesting) tester.stop()
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
        waveExportButton.setOnClickListener { exportWaveformCsv() }
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
        if (steps !in 3..20) { toast("Steps must be 3–20"); return false }
        if (settle !in 100..5000) { toast("Settle time must be 100–5000 ms"); return false }
        if (sample !in 200..8000) { toast("Sample window must be 200–8000 ms"); return false }
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
        val est = (tester.steps * (tester.settleMs + tester.collectMs) / 1000.0).toInt()
        testProgressText.text = "Priming… (~${est}s)"
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
        onTestFinishedUi("Done — R = ${formatOhm(result.resistanceOhm)}")
        val device = simulator?.let { "Demo (%.1f V, %.3f Ω)".format(demoEmf, demoSeriesR) } ?: "EL15 (BLE)"
        ResultActivity.start(this, result, tester.steps, tester.settleMs, tester.collectMs,
            (tester.safetyFactor * 100).toInt(), device, System.currentTimeMillis())
    }

    override fun onTestError(message: String) {
        onTestFinishedUi(null)
        binding.monitor.testProgressText.text = message
        MaterialAlertDialogBuilder(this).setTitle("Test stopped").setMessage(message)
            .setPositiveButton(android.R.string.ok, null).show()
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
        // Stop the tester BEFORE tearing BLE down, and use the safe shutdown
        // path so the final LOAD_OFF is written even mid-sweep.
        val wasTesting = tester.running
        if (wasTesting) {
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
    private fun exportWaveformCsv() {
        val rows = binding.monitor.waveformView.exportRows()
        if (rows.isEmpty()) { toast("No waveform data yet"); return }
        try {
            val dir = File(cacheDir, "shared").apply { mkdirs() }
            val file = File(dir, "waveform.csv")
            FileOutputStream(file).use { out ->
                val sb = StringBuilder("time_ms,elapsed_s,voltage_V,current_A,power_W,temp_C,fan\n")
                val t0 = rows.first().tMs
                for (r in rows) sb.append("%d,%.3f,%.4f,%.4f,%.4f,%.2f,%d\n".format(
                    java.util.Locale.US, r.tMs, (r.tMs - t0) / 1000.0, r.v, r.i, r.p, r.temp, r.fan))
                out.write(sb.toString().toByteArray())
            }
            val uri: Uri = FileProvider.getUriForFile(this, "$packageName.fileprovider", file)
            startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).apply {
                type = "text/csv"; putExtra(Intent.EXTRA_STREAM, uri)
                putExtra(Intent.EXTRA_SUBJECT, "EL15 waveform (${rows.size} samples)")
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }, "Export waveform CSV"))
        } catch (e: Exception) { toast("Export failed: ${e.message}") }
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
        val idle = connected && !tester.running
        val deviceMode = idle && !rtestActive
        for (id in chipToMode.keys) findViewById<Chip>(id).isEnabled = idle
        chipRtest.isEnabled = idle
        setpointInput.isEnabled = deviceMode; setSetpointButton.isEnabled = deviceMode
        lockButton.isEnabled = deviceMode
        // Enabled while disconnected on purpose: tapping it opens the scan
        // dialog (see the click handler). Only a running sweep locks it.
        loadToggle.isEnabled = !tester.running
        fuseInput.isEnabled = idle; stepsInput.isEnabled = idle; settleInput.isEnabled = idle; sampleInput.isEnabled = idle
        startTestButton.isEnabled = !tester.running || connected
    }

    private fun toast(msg: String) = android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_SHORT).show()
}
