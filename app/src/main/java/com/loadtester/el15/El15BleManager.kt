package com.loadtester.el15

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import java.util.ArrayDeque

/**
 * Wraps Android BLE scan + GATT for a single EL15 electronic load.
 *
 * All GATT operations (write, descriptor write) are serialized through a queue
 * because Android's stack only allows one outstanding operation at a time.
 * Callbacks are delivered on the main thread.
 */
@SuppressLint("MissingPermission")
class El15BleManager(private val context: Context) : El15Controller {

    enum class State { IDLE, SCANNING, CONNECTING, CONNECTED }

    interface Listener {
        fun onStateChanged(state: State, info: String)
        fun onDeviceFound(device: BluetoothDevice, name: String)
        fun onStatus(status: El15Status)
        fun onLog(message: String)
    }

    var listener: Listener? = null

    private val main = Handler(Looper.getMainLooper())
    private val adapter: BluetoothAdapter? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    private var gatt: BluetoothGatt? = null
    private var writeChar: BluetoothGattCharacteristic? = null
    private var notifyChar: BluetoothGattCharacteristic? = null

    private val opQueue = ArrayDeque<() -> Unit>()
    private var opInFlight = false

    private val seenDevices = HashSet<String>()
    private var pollRunnable: Runnable? = null

    var state: State = State.IDLE
        private set

    /** Status poll interval in ms; configurable from Settings. */
    var pollIntervalMs: Long = 500L

    val isBluetoothOn: Boolean get() = adapter?.isEnabled == true

    // ---- Scanning ---------------------------------------------------------
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            val name = result.scanRecord?.deviceName ?: device.name ?: "Unknown"
            if (seenDevices.add(device.address)) {
                main.post { listener?.onDeviceFound(device, name) }
            }
        }

        override fun onScanFailed(errorCode: Int) {
            main.post { setState(State.IDLE, "Scan failed ($errorCode)") }
        }
    }

    fun startScan() {
        val scanner = adapter?.bluetoothLeScanner ?: run {
            setState(State.IDLE, "Bluetooth unavailable")
            return
        }
        seenDevices.clear()
        setState(State.SCANNING, "Scanning…")
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(El15Protocol.SERVICE_UUID))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        // Filter by service UUID first; fall back to an unfiltered scan so
        // devices that don't advertise the service still appear.
        scanner.startScan(listOf(filter), settings, scanCallback)
        main.postDelayed({
            if (state == State.SCANNING) stopScanInternal()
        }, SCAN_TIMEOUT_MS)
    }

    fun stopScan() {
        if (state == State.SCANNING) stopScanInternal()
    }

    private fun stopScanInternal() {
        adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        if (state == State.SCANNING) setState(State.IDLE, "Idle")
    }

    // ---- Connection -------------------------------------------------------
    fun connect(device: BluetoothDevice) {
        stopScanInternal()
        setState(State.CONNECTING, "Connecting to ${device.name ?: device.address}…")
        gatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } else {
            device.connectGatt(context, false, gattCallback)
        }
    }

    fun disconnect() {
        stopPolling()
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        writeChar = null
        notifyChar = null
        opQueue.clear()
        opInFlight = false
        setState(State.IDLE, "Disconnected")
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                main.post { setState(State.CONNECTING, "Discovering services…") }
                g.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                main.post {
                    stopPolling()
                    g.close()
                    if (gatt === g) gatt = null
                    setState(State.IDLE, if (status == 0) "Disconnected" else "Disconnected ($status)")
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(El15Protocol.SERVICE_UUID)
            if (service == null) {
                main.post { setState(State.IDLE, "EL15 service not found") }
                g.disconnect()
                return
            }
            writeChar = service.getCharacteristic(El15Protocol.WRITE_UUID)
            notifyChar = service.getCharacteristic(El15Protocol.NOTIFY_UUID)
            if (writeChar == null || notifyChar == null) {
                main.post { setState(State.IDLE, "EL15 characteristics missing") }
                g.disconnect()
                return
            }
            enableNotifications(g, notifyChar!!)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            main.post {
                setState(State.CONNECTED, "Connected")
                startPolling()
                opFinished()
            }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            c: BluetoothGattCharacteristic,
            status: Int
        ) {
            main.post { opFinished() }
        }

        // API <33
        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            handleNotification(c.value)
        }

        // API 33+
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            c: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            handleNotification(value)
        }
    }

    private fun handleNotification(value: ByteArray?) {
        val data = value ?: return
        if (El15Protocol.isStatusPacket(data)) {
            val status = El15Protocol.parseStatus(data)
            main.post { listener?.onStatus(status) }
        }
    }

    private fun enableNotifications(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
        g.setCharacteristicNotification(c, true)
        val ccc = c.getDescriptor(El15Protocol.CCC_UUID) ?: run {
            main.post { setState(State.CONNECTED, "Connected (no CCC)"); startPolling() }
            return
        }
        enqueue {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeDescriptor(ccc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                @Suppress("DEPRECATION")
                ccc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION")
                g.writeDescriptor(ccc)
            }
        }
    }

    // ---- Commands ---------------------------------------------------------
    fun sendCommand(frame: ByteArray) {
        val g = gatt ?: return
        val c = writeChar ?: return
        enqueue {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(c, frame, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
            } else {
                @Suppress("DEPRECATION")
                c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                @Suppress("DEPRECATION")
                c.value = frame
                @Suppress("DEPRECATION")
                g.writeCharacteristic(c)
            }
        }
    }

    override fun setLoad(on: Boolean) = sendCommand(if (on) El15Protocol.LOAD_ON else El15Protocol.LOAD_OFF)
    override fun setLock() = sendCommand(El15Protocol.LOCK)
    override fun setMode(mode: Int) = sendCommand(El15Protocol.modeCommand(mode))
    override fun setSetpoint(value: Float) = sendCommand(El15Protocol.setpointCommand(value))

    // ---- Op queue ---------------------------------------------------------
    private fun enqueue(op: () -> Unit) {
        main.post {
            opQueue.add(op)
            if (!opInFlight) runNext()
        }
    }

    private fun runNext() {
        val op = opQueue.poll() ?: return
        opInFlight = true
        try {
            op()
        } catch (e: Exception) {
            listener?.onLog("Op error: ${e.message}")
            opFinished()
        }
        // Safety: if the callback never fires, unblock after a timeout.
        main.postDelayed({ if (opInFlight) opFinished() }, OP_TIMEOUT_MS)
    }

    private fun opFinished() {
        opInFlight = false
        if (opQueue.isNotEmpty()) runNext()
    }

    // ---- Polling ----------------------------------------------------------
    private fun startPolling() {
        stopPolling()
        val r = object : Runnable {
            override fun run() {
                if (state == State.CONNECTED) {
                    sendCommand(El15Protocol.POLL)
                    main.postDelayed(this, pollIntervalMs)
                }
            }
        }
        pollRunnable = r
        main.post(r)
    }

    private fun stopPolling() {
        pollRunnable?.let { main.removeCallbacks(it) }
        pollRunnable = null
    }

    private fun setState(newState: State, info: String) {
        state = newState
        listener?.onStateChanged(newState, info)
    }

    companion object {
        private const val SCAN_TIMEOUT_MS = 12_000L
        private const val OP_TIMEOUT_MS = 2_000L
    }
}
