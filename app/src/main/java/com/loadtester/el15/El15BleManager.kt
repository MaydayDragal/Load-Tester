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
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.util.ArrayDeque

/**
 * Wraps Android BLE scan + GATT for a single EL15 electronic load.
 *
 * All GATT operations (write, descriptor write) are serialized through a queue
 * because Android's stack only allows one outstanding operation at a time.
 * Callbacks are delivered on the main thread.
 *
 * Safety: [shutdownAndDisconnect] issues a best-effort LOAD_OFF directly on the
 * GATT (bypassing the queue) before tearing the connection down, so leaving the
 * app mid-test cannot strand the load sinking current.
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
    private var servicesRequested = false

    private val opQueue = ArrayDeque<() -> Unit>()
    private var opInFlight = false

    /** Generation counter: each op gets a token so stale timeouts can't unblock a later op. */
    private var opGen = 0L

    private val seenDevices = HashSet<String>()
    private var pollRunnable: Runnable? = null
    private var scanTimeout: Runnable? = null
    private var shuttingDown = false
    private var shutdownOffSent = false

    /** Reassembly buffer for status frames chunked across notifications. */
    private var frameBuf = ByteArray(0)

    var state: State = State.IDLE
        private set

    /** Status poll interval in ms; configurable from Settings. */
    var pollIntervalMs: Long = 500L

    val isBluetoothOn: Boolean get() = adapter?.isEnabled == true

    // ---- Scanning ---------------------------------------------------------
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            // Unfiltered scan: EL15 units often don't put FFF0 in their adverts,
            // so we list every *named* device and verify the service after
            // connecting (onServicesDiscovered rejects non-EL15 devices).
            val name = result.scanRecord?.deviceName ?: device.name ?: return
            if (name.isBlank()) return
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
        stopScanInternal() // restarting an active scan would fail with SCAN_FAILED_ALREADY_STARTED
        seenDevices.clear()
        setState(State.SCANNING, "Scanning…")
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanner.startScan(null, settings, scanCallback)
        val timeout = Runnable { if (state == State.SCANNING) stopScanInternal() }
        scanTimeout = timeout
        main.postDelayed(timeout, SCAN_TIMEOUT_MS)
    }

    fun stopScan() {
        if (state == State.SCANNING) stopScanInternal()
    }

    private fun stopScanInternal() {
        scanTimeout?.let { main.removeCallbacks(it) }
        scanTimeout = null
        adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        if (state == State.SCANNING) setState(State.IDLE, "Idle")
    }

    // ---- Connection -------------------------------------------------------
    fun connect(device: BluetoothDevice) {
        stopScanInternal()
        gatt?.close() // drop any stale connection before opening a new one
        resetSession()
        setState(State.CONNECTING, "Connecting to ${device.name ?: device.address}…")
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    fun disconnect() {
        stopPolling()
        shuttingDown = false
        shutdownOffSent = false
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        resetSession()
        setState(State.IDLE, "Disconnected")
    }

    /**
     * Best-effort safe teardown: writes LOAD_OFF directly on the GATT (bypassing
     * the op queue, which is cleared), then disconnects once the write's
     * callback fires or after a short timeout. Falls back to a plain
     * [disconnect] when there is nothing to write to.
     */
    fun shutdownAndDisconnect() {
        val g = gatt
        val c = writeChar
        if (state != State.CONNECTED || g == null || c == null) {
            disconnect()
            return
        }
        if (shuttingDown) return
        shuttingDown = true
        stopPolling()
        opQueue.clear()
        opGen++ // invalidate any outstanding op timeouts
        opInFlight = false
        // If the stack is busy with an in-flight op this returns false; that
        // op's onCharacteristicWrite retries below.
        shutdownOffSent = writeFrame(g, c, El15Protocol.LOAD_OFF)
        main.postDelayed({ if (shuttingDown) disconnect() }, SHUTDOWN_TIMEOUT_MS)
    }

    private fun resetSession() {
        writeChar = null
        notifyChar = null
        servicesRequested = false
        frameBuf = ByteArray(0)
        opQueue.clear()
        opGen++
        opInFlight = false
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                main.post {
                    setState(State.CONNECTING, "Negotiating…")
                    // 28-byte frames need MTU >= 31; default ATT MTU (23) truncates them.
                    if (!g.requestMtu(64)) requestServices(g)
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                main.post {
                    stopPolling()
                    shuttingDown = false
                    g.close()
                    if (gatt === g) {
                        gatt = null
                        resetSession()
                    }
                    setState(State.IDLE, if (status == 0) "Disconnected" else "Disconnected ($status)")
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            // Proceed regardless of the negotiated value; reassembly below
            // handles chunked delivery if the peripheral splits frames.
            main.post { requestServices(g) }
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
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    setState(State.IDLE, "Notification setup failed ($status)")
                    g.disconnect()
                    return@post
                }
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
            main.post {
                if (shuttingDown) {
                    if (!shutdownOffSent) {
                        // The op that blocked us just completed — send LOAD_OFF now.
                        shutdownOffSent = gatt?.let { gg ->
                            writeChar?.let { cc -> writeFrame(gg, cc, El15Protocol.LOAD_OFF) }
                        } ?: false
                        if (!shutdownOffSent) disconnect()
                    } else {
                        // The safe-shutdown LOAD_OFF itself completed.
                        disconnect()
                    }
                } else {
                    opFinished()
                }
            }
        }

        // API <33
        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION")
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

    private fun requestServices(g: BluetoothGatt) {
        if (servicesRequested) return
        servicesRequested = true
        setState(State.CONNECTING, "Discovering services…")
        g.discoverServices()
    }

    /**
     * Reassemble status frames: some stacks/firmwares deliver a 28-byte frame
     * as one notification, others chunk it. Bytes accumulate until a full frame
     * starting with the DF 07 03 08 header is available; leading garbage is
     * dropped to resynchronize.
     */
    private fun handleNotification(value: ByteArray?) {
        val data = value ?: return
        var buf = if (El15Protocol.isStatusPacket(data)) data else frameBuf + data
        // Resync: drop bytes until the buffer starts with the frame header.
        while (buf.size >= 4 && !El15Protocol.isStatusPacket(buf)) buf = buf.copyOfRange(1, buf.size)
        while (buf.size >= FRAME_LEN) {
            val frame = buf.copyOfRange(0, FRAME_LEN)
            buf = buf.copyOfRange(FRAME_LEN, buf.size)
            val status = El15Protocol.parseStatus(frame)
            main.post { listener?.onStatus(status) }
        }
        frameBuf = if (buf.size > MAX_BUF) ByteArray(0) else buf
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
    /** Issue a write directly on the GATT; returns false if the stack rejected it. */
    private fun writeFrame(g: BluetoothGatt, c: BluetoothGattCharacteristic, frame: ByteArray): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(c, frame, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) ==
                android.bluetooth.BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            @Suppress("DEPRECATION")
            c.value = frame
            @Suppress("DEPRECATION")
            g.writeCharacteristic(c)
        }
    }

    fun sendCommand(frame: ByteArray) {
        enqueue {
            val g = gatt
            val c = writeChar
            if (g == null || c == null) {
                opFinished()
                return@enqueue
            }
            if (!writeFrame(g, c, frame)) {
                listener?.onLog("Write rejected by stack")
                opFinished()
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
            if (shuttingDown) return@post
            opQueue.add(op)
            if (!opInFlight) runNext()
        }
    }

    private fun runNext() {
        val op = opQueue.poll() ?: return
        opInFlight = true
        val gen = ++opGen
        try {
            op()
        } catch (e: Exception) {
            listener?.onLog("Op error: ${e.message}")
            opFinished()
            return
        }
        // Safety net: if the callback never fires for THIS op, unblock. The
        // generation token keeps a stale timeout from unblocking a later op.
        main.postDelayed({ if (opInFlight && opGen == gen) opFinished() }, OP_TIMEOUT_MS)
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
                if (state == State.CONNECTED && !shuttingDown) {
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
        private const val SHUTDOWN_TIMEOUT_MS = 500L
        private const val FRAME_LEN = 28
        private const val MAX_BUF = 128
    }
}
