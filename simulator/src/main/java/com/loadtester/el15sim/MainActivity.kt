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

        binding.advButton.setOnClickListener {
            if (server.isAdvertising) { server.stop(); afterAdvertisingChanged(false, "Stopped") }
            else requestAndStart()
        }
        binding.applyButton.setOnClickListener { applyCircuit() }
        binding.clearLogButton.setOnClickListener { logLines.clear(); binding.logView.text = "" }

        refreshState()
    }

    override fun onDestroy() {
        super.onDestroy()
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
        if (!server.isPeripheralCapable) {
            binding.advStatus.text = "This phone can't act as a BLE peripheral"
        }
        server.start()
    }

    // ---- Circuit editing ---------------------------------------------------
    private fun applyCircuit() {
        model.emf = binding.emfInput.text.toString().trim().toFloatOrNull()?.coerceIn(0.1f, 100f) ?: model.emf
        model.seriesR = binding.resInput.text.toString().trim().toFloatOrNull()?.coerceIn(0f, 100f) ?: model.seriesR
        binding.emfInput.setText(fmt(model.emf))
        binding.resInput.setText(fmt(model.seriesR))
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
        binding.stateSub.text =
            "mode $modeName · setpoint %.3f · load %s · lock %s%s".format(
                model.setpoint,
                if (model.loadOn) "ON" else "off",
                if (model.lockOn) "ON" else "off",
                warn,
            )
    }

    private fun log(msg: String) {
        val stamp = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        logLines.addFirst("$stamp  $msg")
        while (logLines.size > 60) logLines.removeLast()
        binding.logView.text = logLines.joinToString("\n")
    }

    private fun fmt(v: Float) = if (v == v.toLong().toFloat()) v.toLong().toString() else v.toString()
}
