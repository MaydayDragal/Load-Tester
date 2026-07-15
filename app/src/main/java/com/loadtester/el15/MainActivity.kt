package com.loadtester.el15

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.ArrayAdapter
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.loadtester.el15.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity(), El15BleManager.Listener,
    CircuitResistanceTester.Callback {

    private lateinit var binding: ActivityMainBinding
    private lateinit var ble: El15BleManager
    private lateinit var tester: CircuitResistanceTester

    private val foundDevices = LinkedHashMap<String, BluetoothDevice>()
    private var lastStatus: El15Status? = null
    private var selectedMode: Int = El15Protocol.MODE_CC
    private var userEditingSetpoint = false

    /** Non-null while the demo device is "connected". */
    private var simulator: El15Simulator? = null

    /** The load currently being driven: the demo device if active, else BLE. */
    private val controller: El15Controller get() = simulator ?: ble

    /** Stable handle for the tester that always forwards to the active controller. */
    private val activeController = object : El15Controller {
        override fun setMode(mode: Int) = controller.setMode(mode)
        override fun setSetpoint(value: Float) = controller.setSetpoint(value)
        override fun setLoad(on: Boolean) = controller.setLoad(on)
        override fun setLock() = controller.setLock()
    }

    private fun isConnected(): Boolean =
        simulator != null || ble.state == El15BleManager.State.CONNECTED

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result.values.all { it }) {
            startBleScan()
        } else {
            toast("Bluetooth permission denied — the demo device is still available")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        ble = El15BleManager(this).also { it.listener = this }
        tester = CircuitResistanceTester(activeController, this)

        setupModeSpinner()
        setupControls()
        setupResistanceTest()
        updateControlsEnabled(false)
    }

    override fun onDestroy() {
        super.onDestroy()
        if (tester.running) tester.stop()
        simulator?.stop()
        ble.disconnect()
    }

    // ---- UI setup ---------------------------------------------------------
    private fun setupModeSpinner() {
        val names = El15Protocol.SELECTABLE_MODES.map { El15Protocol.MODE_NAMES[it] ?: "?" }
        binding.modeSpinner.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item, names
        )
        binding.setModeButton.setOnClickListener {
            val idx = binding.modeSpinner.selectedItemPosition
            val mode = El15Protocol.SELECTABLE_MODES[idx]
            selectedMode = mode
            controller.setMode(mode)
            toast("Mode → ${El15Protocol.MODE_NAMES[mode]}")
        }
    }

    private fun setupControls() {
        binding.connectButton.setOnClickListener {
            if (isConnected() || ble.state == El15BleManager.State.CONNECTING) {
                disconnectAll()
            } else {
                requestScan()
            }
        }

        binding.setpointInput.setOnFocusChangeListener { _, hasFocus ->
            userEditingSetpoint = hasFocus
        }

        binding.setSetpointButton.setOnClickListener {
            val value = binding.setpointInput.text.toString().trim().toFloatOrNull()
            if (value == null) {
                toast("Enter a valid number")
            } else {
                controller.setSetpoint(value)
                binding.setpointInput.clearFocus()
                toast("Setpoint → $value")
            }
        }

        binding.loadToggle.setOnClickListener {
            val on = lastStatus?.loadOn ?: false
            controller.setLoad(!on)
        }

        binding.lockButton.setOnClickListener { controller.setLock() }
    }

    // ---- Circuit resistance test -----------------------------------------
    private fun setupResistanceTest() {
        // Prefill sweep options with the engine defaults.
        if (binding.stepsInput.text.isNullOrBlank()) binding.stepsInput.setText(tester.steps.toString())
        if (binding.settleInput.text.isNullOrBlank()) binding.settleInput.setText(tester.settleMs.toString())
        if (binding.sampleInput.text.isNullOrBlank()) binding.sampleInput.setText(tester.collectMs.toString())

        binding.startTestButton.setOnClickListener {
            if (tester.running) {
                tester.stop()
                onTestFinishedUi("Test stopped")
                return@setOnClickListener
            }
            if (!isConnected()) {
                toast("Connect to the EL15 (or the demo device) first")
                return@setOnClickListener
            }
            val fuse = binding.fuseInput.text.toString().trim().toFloatOrNull()
            if (fuse == null || fuse <= 0f) {
                toast("Enter the circuit's fuse rating in amps")
                return@setOnClickListener
            }
            if (fuse > 200f) {
                toast("That fuse rating looks too high — double-check the value")
                return@setOnClickListener
            }
            if (!applyTestOptions()) return@setOnClickListener
            val (peak, limiter) = predictedPeak(fuse)
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.rt_confirm_title)
                .setMessage(getString(R.string.rt_confirm_msg, peak, limiter))
                .setPositiveButton(R.string.rt_start) { _, _ -> startTest(fuse) }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }
    }

    /**
     * Estimate the peak current the sweep will reach and what limits it, using
     * the present (open-circuit) voltage. The tester re-derives this precisely
     * after priming; this is only for the confirmation dialog.
     */
    private fun predictedPeak(fuse: Float): Pair<Float, String> {
        val v = lastStatus?.voltage?.takeIf { it > El15Protocol.MIN_VOLTAGE_V }
            ?: El15Protocol.MAX_VOLTAGE_V
        val fuseCap = fuse * tester.safetyFactor
        val powerCap = El15Protocol.MAX_POWER_W / v
        val peak = minOf(fuseCap, powerCap, El15Protocol.MAX_CURRENT_A)
        val limiter = when (peak) {
            fuseCap -> "80%% of the %.1f A fuse".format(fuse)
            powerCap -> "the %.0f W power limit at %.1f V".format(El15Protocol.MAX_POWER_W, v)
            else -> "the %.0f A current limit".format(El15Protocol.MAX_CURRENT_A)
        }
        return peak to limiter
    }

    /** Read the user-configurable sweep options; returns false and toasts if invalid. */
    private fun applyTestOptions(): Boolean {
        val steps = binding.stepsInput.text.toString().trim().toIntOrNull() ?: DEFAULT_STEPS
        val settle = binding.settleInput.text.toString().trim().toLongOrNull() ?: DEFAULT_SETTLE_MS
        val sample = binding.sampleInput.text.toString().trim().toLongOrNull() ?: DEFAULT_SAMPLE_MS
        if (steps !in 3..20) { toast("Steps must be between 3 and 20"); return false }
        if (settle !in 100..5000) { toast("Settle time must be 100–5000 ms"); return false }
        if (sample !in 200..8000) { toast("Sample window must be 200–8000 ms"); return false }
        tester.steps = steps
        tester.settleMs = settle
        tester.collectMs = sample
        // Reflect any clamping back into the fields.
        binding.stepsInput.setText(steps.toString())
        binding.settleInput.setText(settle.toString())
        binding.sampleInput.setText(sample.toString())
        return true
    }

    private fun startTest(fuse: Float) {
        binding.fuseInput.clearFocus()
        binding.testProgressText.visibility = View.VISIBLE
        val est = (tester.steps * (tester.settleMs + tester.collectMs) / 1000.0).toInt()
        binding.testProgressText.text = "Priming… (~${est}s)"
        binding.startTestButton.setText(R.string.rt_stop)
        updateControlsEnabled(true) // refresh: disables manual controls while testing
        tester.start(fuse)
    }

    private fun onTestFinishedUi(message: String?) {
        binding.startTestButton.setText(R.string.rt_start)
        message?.let { binding.testProgressText.text = it }
        updateControlsEnabled(isConnected())
    }

    override fun onTestProgress(step: Int, totalSteps: Int, targetCurrent: Float, voltage: Float, current: Float) {
        binding.testProgressText.text =
            "Step %d/%d  →  %.3f A set   %.3f V  @ %.3f A".format(step, totalSteps, targetCurrent, voltage, current)
    }

    override fun onTestComplete(result: CircuitResistanceTester.ResistanceResult) {
        onTestFinishedUi("Done — R = ${formatOhm(result.resistanceOhm)}")
        ResultActivity.start(this, result)
    }

    override fun onTestError(message: String) {
        onTestFinishedUi(null)
        binding.testProgressText.text = message
        MaterialAlertDialogBuilder(this)
            .setTitle("Test stopped")
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun formatOhm(ohm: Float): String = when {
        ohm <= 0f -> "n/a"
        ohm < 1f -> "%.1f mΩ".format(ohm * 1000f)
        else -> "%.3f Ω".format(ohm)
    }

    // ---- Permissions / scanning ------------------------------------------
    private fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    private fun hasPermissions(): Boolean = requiredPermissions().all {
        ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
    }

    private fun requestScan() {
        // The picker always offers the demo device; real scanning is added when
        // Bluetooth is on and permitted.
        showDevicePicker()
        when {
            !ble.isBluetoothOn ->
                toast("Bluetooth is off — the demo device is still available")
            hasPermissions() -> startBleScan()
            else -> permissionLauncher.launch(requiredPermissions())
        }
    }

    private fun startBleScan() {
        if (deviceDialog?.isShowing != true) return
        foundDevices.clear()
        ble.startScan()
    }

    private var deviceDialog: AlertDialog? = null
    private lateinit var deviceListAdapter: ArrayAdapter<String>

    @SuppressLint("MissingPermission")
    private fun showDevicePicker() {
        foundDevices.clear()
        deviceListAdapter = ArrayAdapter(this, android.R.layout.simple_list_item_1)
        deviceListAdapter.add(getString(R.string.demo_device)) // always position 0
        deviceDialog = MaterialAlertDialogBuilder(this)
            .setTitle("Select device")
            .setAdapter(deviceListAdapter) { _, which ->
                if (which == 0) {
                    startSimulator()
                } else {
                    val device = foundDevices.values.toList().getOrNull(which - 1)
                    if (device != null) ble.connect(device)
                }
            }
            .setNegativeButton("Cancel") { _, _ -> ble.stopScan() }
            .setOnDismissListener { ble.stopScan() }
            .show()
    }

    // ---- Demo device ------------------------------------------------------
    private fun startSimulator() {
        ble.stopScan()
        deviceDialog?.dismiss()
        val sim = El15Simulator({ status -> runOnUiThread { handleIncomingStatus(status) } })
        simulator = sim
        sim.start()
        binding.statusText.text = getString(R.string.demo_connected)
        binding.connectButton.text = getString(R.string.disconnect)
        clearReadouts()
        updateControlsEnabled(true)
    }

    private fun stopSimulator() {
        if (tester.running) tester.stop()
        simulator?.stop()
        simulator = null
        binding.statusText.text = "Disconnected"
        binding.connectButton.text = getString(R.string.scan_connect)
        updateControlsEnabled(false)
        clearReadouts()
    }

    private fun disconnectAll() {
        if (simulator != null) stopSimulator() else ble.disconnect()
    }

    private fun clearReadouts() {
        binding.voltageValue.text = "—"
        binding.currentValue.text = "—"
        binding.powerValue.text = "—"
        binding.modeValue.text = "—"
        binding.extraValue.text = ""
        binding.warningText.visibility = View.GONE
    }

    // ---- BLE listener callbacks ------------------------------------------
    override fun onStateChanged(state: El15BleManager.State, info: String) {
        if (simulator != null) return // demo device active; ignore BLE state
        binding.statusText.text = info
        val connected = state == El15BleManager.State.CONNECTED
        if (!connected && tester.running) {
            tester.stop()
            onTestFinishedUi("Test stopped (disconnected)")
        }
        binding.connectButton.text = when (state) {
            El15BleManager.State.CONNECTED -> getString(R.string.disconnect)
            El15BleManager.State.CONNECTING -> getString(R.string.cancel)
            else -> getString(R.string.scan_connect)
        }
        updateControlsEnabled(connected)
        if (state != El15BleManager.State.SCANNING) deviceDialog?.dismiss()
        if (!connected) clearReadouts()
    }

    @SuppressLint("MissingPermission")
    override fun onDeviceFound(device: BluetoothDevice, name: String) {
        if (foundDevices.put(device.address, device) == null) {
            deviceListAdapter.add("$name\n${device.address}")
            deviceListAdapter.notifyDataSetChanged()
        }
    }

    override fun onStatus(status: El15Status) = handleIncomingStatus(status)

    /** Shared handler for status updates from either BLE or the demo device. */
    private fun handleIncomingStatus(status: El15Status) {
        lastStatus = status
        if (tester.running) tester.onStatus(status)
        binding.voltageValue.text = "%.3f V".format(status.voltage)
        binding.modeValue.text = status.modeName

        if (status.mode == El15Protocol.MODE_DCR) {
            binding.currentValue.text = "%.3f A".format(status.dcrI1)
            binding.powerValue.text = "—"
        } else {
            binding.currentValue.text = "%.3f A".format(status.current)
            binding.powerValue.text = "%.3f W".format(status.power)
        }

        binding.extraValue.text = buildExtraLine(status)

        // Load toggle reflects device state.
        binding.loadToggle.text =
            if (status.loadOn) getString(R.string.load_off) else getString(R.string.load_on)
        binding.loadToggle.isSelected = status.loadOn

        // Warning / protection banner.
        if (status.warning.isNotEmpty()) {
            binding.warningText.visibility = View.VISIBLE
            binding.warningText.text = getString(R.string.protection, status.warning)
        } else {
            binding.warningText.visibility = View.GONE
        }

        // Setpoint hint (unit + label) and value, unless the user is typing.
        binding.setpointLayout.hint = "${status.setpointLabel} (${status.setpointUnit})"
        if (!userEditingSetpoint && status.setpointInPacket) {
            binding.setpointInput.setText(
                "%.${status.setpointDecimals}f".format(status.setpoint)
            )
        }
    }

    private fun buildExtraLine(s: El15Status): String {
        val parts = mutableListOf<String>()
        when (s.mode) {
            El15Protocol.MODE_CAP -> {
                parts += "Energy %.3f Wh".format(s.energyWh)
                parts += "Capacity %.3f Ah".format(s.capacityAh)
            }
            El15Protocol.MODE_DCR -> {
                parts += "R %.1f mΩ".format(s.dcrMilliOhm)
                parts += "I2 %.3f A".format(s.dcrI2)
            }
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
        val h = seconds / 3600
        val m = (seconds % 3600) / 60
        val sec = seconds % 60
        return if (h > 0) "%d:%02d:%02d".format(h, m, sec) else "%02d:%02d".format(m, sec)
    }

    override fun onLog(message: String) {
        // Surface only unexpected issues.
    }

    private fun updateControlsEnabled(connected: Boolean) {
        // Manual controls are available only while connected AND no test is running.
        val manual = connected && !tester.running
        binding.modeSpinner.isEnabled = manual
        binding.setModeButton.isEnabled = manual
        binding.setpointInput.isEnabled = manual
        binding.setSetpointButton.isEnabled = manual
        binding.loadToggle.isEnabled = manual
        binding.lockButton.isEnabled = manual
        // The test can be started only while connected; the button doubles as Stop.
        binding.startTestButton.isEnabled = connected
        binding.fuseInput.isEnabled = connected && !tester.running
    }

    private fun toast(msg: String) {
        android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_SHORT).show()
    }

    companion object {
        private const val DEFAULT_STEPS = 8
        private const val DEFAULT_SETTLE_MS = 800L
        private const val DEFAULT_SAMPLE_MS = 1500L
    }
}
