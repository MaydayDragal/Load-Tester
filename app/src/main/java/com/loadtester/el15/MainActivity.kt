package com.loadtester.el15

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.provider.Settings
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

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result.values.all { it }) {
            beginScan()
        } else {
            toast("Bluetooth permissions are required to connect")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        ble = El15BleManager(this).also { it.listener = this }
        tester = CircuitResistanceTester(ble, this)

        setupModeSpinner()
        setupControls()
        setupResistanceTest()
        updateControlsEnabled(false)
    }

    override fun onDestroy() {
        super.onDestroy()
        if (tester.running) tester.stop()
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
            ble.setMode(mode)
            toast("Mode → ${El15Protocol.MODE_NAMES[mode]}")
        }
    }

    private fun setupControls() {
        binding.connectButton.setOnClickListener {
            when (ble.state) {
                El15BleManager.State.CONNECTED,
                El15BleManager.State.CONNECTING -> ble.disconnect()
                else -> requestScan()
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
                ble.setSetpoint(value)
                binding.setpointInput.clearFocus()
                toast("Setpoint → $value")
            }
        }

        binding.loadToggle.setOnClickListener {
            val on = lastStatus?.loadOn ?: false
            ble.setLoad(!on)
        }

        binding.lockButton.setOnClickListener { ble.setLock() }
    }

    // ---- Circuit resistance test -----------------------------------------
    private fun setupResistanceTest() {
        binding.startTestButton.setOnClickListener {
            if (tester.running) {
                tester.stop()
                onTestFinishedUi("Test stopped")
                return@setOnClickListener
            }
            if (ble.state != El15BleManager.State.CONNECTED) {
                toast("Connect to the EL15 first")
                return@setOnClickListener
            }
            val fuse = binding.fuseInput.text.toString().trim().toFloatOrNull()
            if (fuse == null || fuse <= 0f) {
                toast("Enter the circuit's fuse rating in amps")
                return@setOnClickListener
            }
            if (fuse > CircuitResistanceTester.ABS_MAX_CURRENT / tester.safetyFactor) {
                toast("Fuse rating too high (max ${CircuitResistanceTester.ABS_MAX_CURRENT} A test current)")
                return@setOnClickListener
            }
            val maxI = fuse * tester.safetyFactor
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.rt_confirm_title)
                .setMessage(getString(R.string.rt_confirm_msg, maxI, fuse))
                .setPositiveButton(R.string.rt_start) { _, _ -> startTest(fuse) }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }
    }

    private fun startTest(fuse: Float) {
        binding.fuseInput.clearFocus()
        binding.testProgressText.visibility = View.VISIBLE
        binding.testProgressText.text = "Priming…"
        binding.startTestButton.setText(R.string.rt_stop)
        updateControlsEnabled(true) // refresh: disables manual controls while testing
        tester.start(fuse)
    }

    private fun onTestFinishedUi(message: String?) {
        binding.startTestButton.setText(R.string.rt_start)
        message?.let { binding.testProgressText.text = it }
        updateControlsEnabled(ble.state == El15BleManager.State.CONNECTED)
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
        if (!ble.isBluetoothOn) {
            MaterialAlertDialogBuilder(this)
                .setTitle("Bluetooth is off")
                .setMessage("Enable Bluetooth to scan for the EL15 load.")
                .setPositiveButton("Settings") { _, _ ->
                    startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
                }
                .setNegativeButton("Cancel", null)
                .show()
            return
        }
        if (hasPermissions()) beginScan() else permissionLauncher.launch(requiredPermissions())
    }

    private fun beginScan() {
        foundDevices.clear()
        ble.startScan()
        showDevicePicker()
    }

    private var deviceDialog: AlertDialog? = null
    private lateinit var deviceListAdapter: ArrayAdapter<String>

    @SuppressLint("MissingPermission")
    private fun showDevicePicker() {
        deviceListAdapter = ArrayAdapter(this, android.R.layout.simple_list_item_1)
        deviceDialog = MaterialAlertDialogBuilder(this)
            .setTitle("Select EL15 device")
            .setAdapter(deviceListAdapter) { _, which ->
                val device = foundDevices.values.toList().getOrNull(which)
                if (device != null) ble.connect(device)
            }
            .setNegativeButton("Cancel") { _, _ -> ble.stopScan() }
            .setOnDismissListener { ble.stopScan() }
            .show()
    }

    // ---- BLE listener callbacks ------------------------------------------
    override fun onStateChanged(state: El15BleManager.State, info: String) {
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
        if (!connected) {
            binding.voltageValue.text = "—"
            binding.currentValue.text = "—"
            binding.powerValue.text = "—"
            binding.modeValue.text = "—"
            binding.extraValue.text = ""
            binding.warningText.visibility = View.GONE
        }
    }

    @SuppressLint("MissingPermission")
    override fun onDeviceFound(device: BluetoothDevice, name: String) {
        if (foundDevices.put(device.address, device) == null) {
            deviceListAdapter.add("$name\n${device.address}")
            deviceListAdapter.notifyDataSetChanged()
        }
    }

    override fun onStatus(status: El15Status) {
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
}
