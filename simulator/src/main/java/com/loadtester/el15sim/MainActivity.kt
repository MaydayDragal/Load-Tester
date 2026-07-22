package com.loadtester.el15sim

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.loadtester.el15sim.databinding.ActivityMainBinding
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Standalone EL15 load simulator. Runs a real BLE GATT peripheral advertising
 * the FFF0 service, so the ESP32 controller (or the phone app) can connect to
 * it over a genuine Bluetooth link and be exercised end-to-end without the load
 * hardware. Shows the live simulated state and a log of the commands received.
 */
class MainActivity : AppCompatActivity(), El15GattServer.Listener {

    private lateinit var binding: ActivityMainBinding
    private val model = LoadModel()
    private lateinit var server: El15GattServer

    private val logLines = ArrayDeque<String>()
    private var connectedAddress: String? = null

    /** True once the user edits the SoC field; cleared when its value is applied. */
    private var socDirty = false

    /** Stops the peripheral cleanly if the user turns Bluetooth off. */
    private val btStateReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(c: android.content.Context?, intent: android.content.Intent?) {
            val state = intent?.getIntExtra(android.bluetooth.BluetoothAdapter.EXTRA_STATE,
                android.bluetooth.BluetoothAdapter.ERROR) ?: return
            if (state == android.bluetooth.BluetoothAdapter.STATE_TURNING_OFF ||
                state == android.bluetooth.BluetoothAdapter.STATE_OFF
            ) {
                if (server.isAdvertising) {
                    server.stop()
                    afterAdvertisingChanged(false, "Bluetooth turned off")
                    log("✕ Bluetooth turned off — simulator stopped")
                }
            }
        }
    }

    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result.values.all { it }) startServer()
        else binding.advStatus.text = getString(R.string.perm_needed)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        server = El15GattServer(this, model).also { it.listener = this }

        binding.emfInput.setText(fmt(model.emf))
        binding.resInput.setText(fmt(model.seriesR))
        binding.nameInput.setText("EL15-SIM")
        binding.cellsInput.setText(model.cells.toString())
        binding.capacityInput.setText(fmt(model.batteryCapacityAh))
        binding.batteryRInput.setText(fmt(model.batteryR))
        binding.socInput.setText("100")
        binding.socInput.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: android.text.Editable?) { socDirty = true }
        })

        binding.chemDropdown.setAdapter(android.widget.ArrayAdapter(
            this, android.R.layout.simple_list_item_1, LoadModel.CHEM_NAMES))
        binding.chemDropdown.setText(LoadModel.CHEM_NAMES[model.chemistry], false)
        binding.chemDropdown.setOnItemClickListener { _, _, _, _ -> updateBatteryFields() }

        binding.leadVoltDropdown.setAdapter(android.widget.ArrayAdapter(
            this, android.R.layout.simple_list_item_1, LEAD_VOLTAGES))
        binding.leadVoltDropdown.setText(LEAD_VOLTAGES[1], false)  // 12 V default
        updateBatteryFields()

        binding.sourceToggle.check(if (model.batteryMode) R.id.srcBattery else R.id.srcFixed)
        binding.sourceToggle.addOnButtonCheckedListener { _, checkedId, isChecked ->
            if (!isChecked) return@addOnButtonCheckedListener
            model.batteryMode = checkedId == R.id.srcBattery
            binding.fixedSection.visibility =
                if (model.batteryMode) android.view.View.GONE else android.view.View.VISIBLE
            binding.batterySection.visibility =
                if (model.batteryMode) android.view.View.VISIBLE else android.view.View.GONE
            refreshState()
        }

        binding.advButton.setOnClickListener {
            if (server.isAdvertising) { server.stop(); afterAdvertisingChanged(false, "Stopped") }
            else requestAndStart()
        }
        binding.applyButton.setOnClickListener { applyCircuit() }
        binding.rechargeButton.setOnClickListener {
            model.recharge()
            binding.socInput.setText("100")
            // The programmatic setText above fires the TextWatcher; clear the
            // dirty flag so a later Apply doesn't silently recharge again.
            socDirty = false
            log("⚡ battery recharged to 100%")
            refreshState()
        }
        binding.clearLogButton.setOnClickListener { logLines.clear(); binding.logView.text = "" }

        registerReceiver(btStateReceiver, android.content.IntentFilter(
            android.bluetooth.BluetoothAdapter.ACTION_STATE_CHANGED))

        refreshState()
    }

    override fun onDestroy() {
        super.onDestroy()
        try { unregisterReceiver(btStateReceiver) } catch (ignored: Exception) {}
        // Detach first: GATT callbacks already queued on the main handler must
        // not call back into a destroyed Activity.
        server.listener = null
        server.stop()
    }

    // ---- Permissions / start ----------------------------------------------
    private fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            arrayOf(Manifest.permission.BLUETOOTH_ADVERTISE, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf()  // pre-31 advertising needs no runtime permission

    private fun hasPermissions() = requiredPermissions().all {
        ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
    }

    private fun requestAndStart() {
        if (hasPermissions()) startServer()
        else permLauncher.launch(requiredPermissions())
    }

    private fun startServer() {
        applyCircuit()
        // Only meaningful with Bluetooth on — the capability flag also reads
        // false while the radio is off, which would show the wrong message.
        if (server.isBluetoothOn && !server.isPeripheralCapable) {
            binding.advStatus.text = "This phone can't act as a BLE peripheral"
        }
        server.start()
    }

    /** Lead-acid is configured by nominal voltage, everything else by cells. */
    private fun updateBatteryFields() {
        val lead = binding.chemDropdown.text.toString() == LoadModel.CHEM_NAMES[LoadModel.CHEM_LEAD]
        binding.cellsLayout.visibility = if (lead) android.view.View.GONE else android.view.View.VISIBLE
        binding.leadVoltLayout.visibility = if (lead) android.view.View.VISIBLE else android.view.View.GONE
    }

    // ---- Source editing ----------------------------------------------------
    private fun applyCircuit() {
        // Fixed circuit.
        model.emf = binding.emfInput.text.toString().trim().toFloatOrNull()?.coerceIn(0.1f, 100f) ?: model.emf
        model.seriesR = binding.resInput.text.toString().trim().toFloatOrNull()?.coerceIn(0f, 100f) ?: model.seriesR
        binding.emfInput.setText(fmt(model.emf))
        binding.resInput.setText(fmt(model.seriesR))

        // Battery pack.
        val chemIdx = LoadModel.CHEM_NAMES.indexOf(binding.chemDropdown.text.toString())
        if (chemIdx >= 0) model.chemistry = chemIdx
        model.cells = if (model.chemistry == LoadModel.CHEM_LEAD) {
            // Lead-acid is picked as a 6 V or 12 V battery (3 or 6 internal cells).
            if (binding.leadVoltDropdown.text.toString().startsWith("6")) 3 else 6
        } else {
            binding.cellsInput.text.toString().trim().toIntOrNull()?.coerceIn(1, 30) ?: model.cells
        }
        model.batteryCapacityAh = binding.capacityInput.text.toString().trim().toFloatOrNull()
            ?.coerceIn(0.01f, 1000f) ?: model.batteryCapacityAh
        model.batteryR = binding.batteryRInput.text.toString().trim().toFloatOrNull()
            ?.coerceIn(0f, 100f) ?: model.batteryR
        // Don't write the lead-acid 3/6 mapping into the (hidden) cells field —
        // it holds the user's cell count for the other chemistries.
        if (model.chemistry != LoadModel.CHEM_LEAD) {
            binding.cellsInput.setText(model.cells.toString())
        }
        binding.capacityInput.setText(fmt(model.batteryCapacityAh))
        binding.batteryRInput.setText(fmt(model.batteryR))
        // Only reset the battery's state of charge if the user actually edited
        // the field — re-applying other settings mid-discharge must not
        // silently "recharge" the pack.
        if (socDirty) {
            val pct = binding.socInput.text.toString().trim().toFloatOrNull()
            if (pct != null) {
                model.recharge(pct.coerceIn(0f, 100f))
                binding.socInput.setText(fmt(pct.coerceIn(0f, 100f)))
            } else {
                // Unparseable/blank: don't guess — show the live SoC instead.
                binding.socInput.setText(fmt((model.soc * 100.0).toFloat()))
            }
            socDirty = false
        }

        val name = binding.nameInput.text.toString().trim()
        if (name.isNotEmpty()) server.setAdvertisedName(name)
        refreshState()
    }

    // ---- Listener ----------------------------------------------------------
    override fun onAdvertising(active: Boolean, error: String?) {
        afterAdvertisingChanged(active, error ?: if (active) "Advertising as EL15" else "Idle")
    }

    private fun afterAdvertisingChanged(active: Boolean, status: String) {
        binding.advStatus.text = status
        binding.advDot.backgroundTintList = android.content.res.ColorStateList.valueOf(
            ContextCompat.getColor(this, if (active) R.color.green else R.color.red))
        binding.advButton.text = getString(if (active) R.string.stop_adv else R.string.start_adv)
        binding.advButton.backgroundTintList = android.content.res.ColorStateList.valueOf(
            ContextCompat.getColor(this, if (active) R.color.red else R.color.green))
        // Hold the screen while acting as the load: with the screen off, Doze /
        // process death could silently end an hours-long simulated discharge.
        if (active) window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    override fun onCentralConnected(address: String) {
        connectedAddress = address
        binding.connInfo.text = "Central connected: $address"
        log("↔ connected $address")
    }

    override fun onCentralDisconnected(address: String) {
        if (connectedAddress == address) connectedAddress = null
        binding.connInfo.text = if (connectedAddress == null) "No central connected"
        else "Central connected: $connectedAddress"
        log("✕ disconnected $address")
    }

    override fun onCommand(description: String) {
        log("→ $description")
        refreshState()
    }

    override fun onNotified(voltage: Float, current: Float, warning: String) {
        refreshState()
    }

    // ---- Rendering ---------------------------------------------------------
    private fun refreshState() {
        val modeName = LoadModel.MODE_NAMES[model.mode] ?: "?"
        val power = model.lastVoltage * model.lastCurrent
        binding.stateReadout.text =
            "%.2f V   %.3f A   %.1f W".format(model.lastVoltage, model.lastCurrent, power)
        val warn = if (model.lastWarning.isNotEmpty()) "  ⚠ ${model.lastWarning}" else ""
        val battery = if (model.batteryMode)
            "\nbattery SoC %.1f%% · OCV %.2f V · drawn %.3f / %s Ah".format(
                model.soc * 100.0, model.ocvNow(), model.drawnAh, fmt(model.batteryCapacityAh))
        else ""
        binding.stateSub.text =
            "mode $modeName · setpoint %.3f · load %s · lock %s%s%s".format(
                model.setpoint,
                if (model.loadOn) "ON" else "off",
                if (model.lockOn) "ON" else "off",
                warn,
                battery,
            )
    }

    private fun log(msg: String) {
        val stamp = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        logLines.addFirst("$stamp  $msg")
        while (logLines.size > 60) logLines.removeLast()
        binding.logView.text = logLines.joinToString("\n")
    }

    private fun fmt(v: Float) = if (v == v.toLong().toFloat()) v.toLong().toString() else v.toString()

    companion object {
        private val LEAD_VOLTAGES = listOf("6 V", "12 V")
    }
}
