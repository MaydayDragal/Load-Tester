package com.loadtester.el15sim

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.SystemClock
import java.util.UUID

/**
 * Advertises the EL15's FFF0 service and runs a GATT server that behaves like
 * the real load: it accepts command writes on FFF3, and pushes 28-byte status
 * notifications on FFF1 (on every poll, on any state change, and on a steady
 * timer so the load's accumulators keep advancing while the ESP is connected).
 *
 * Threading: Android delivers GATT server + advertise callbacks on binder
 * threads. Every callback here does the minimum — copy its payload, answer
 * sendResponse (a thread-safe API), and post the real work to the main
 * handler — so the [LoadModel], subscriber set, notify characteristic, send
 * queue, and all [Listener] callbacks are main-thread-confined. Reads are
 * served from volatile snapshots.
 *
 * Lifecycle: [start]/[stop] are guarded by a synchronous [started] intent flag
 * (the async advertise result callbacks alone cannot be trusted for re-entry
 * guarding — see the double-start / zombie-advert defect class). Advertising
 * is re-armed after a central disconnects, since many stacks stop legacy
 * advertising on connection.
 *
 * Notifications are chunked to each central's negotiated ATT MTU (payload cap
 * is MTU−3) and paced by onNotificationSent, so a central that never raises
 * the default 23-byte MTU still receives the full 28-byte frame split across
 * notifications — both the ESP and phone-app centrals reassemble splits.
 *
 * All Bluetooth calls need BLUETOOTH_ADVERTISE + BLUETOOTH_CONNECT (API 31+);
 * the Activity gates on those before [start] is called.
 */
@SuppressLint("MissingPermission")
class El15GattServer(private val context: Context, private val model: LoadModel) {

    interface Listener {
        fun onAdvertising(active: Boolean, error: String?)
        fun onCentralConnected(address: String)
        fun onCentralDisconnected(address: String)
        fun onCommand(description: String)
        fun onNotified(voltage: Float, current: Float, warning: String)
    }

    var listener: Listener? = null

    /** How often to push an unsolicited status frame (keeps accumulators moving). */
    var pushIntervalMs: Long = 500L

    private val main = Handler(Looper.getMainLooper())
    private val btManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val adapter get() = btManager.adapter

    private var server: BluetoothGattServer? = null
    private var notifyChar: BluetoothGattCharacteristic? = null
    private val subscribed = LinkedHashSet<BluetoothDevice>()
    private val mtus = HashMap<String, Int>()

    /** Synchronous "user wants the peripheral running" flag (main thread). */
    private var started = false
    /** True once the stack confirms the advert is on air (main thread). */
    private var advertising = false

    /** Last encoded status frame; read requests answer from this on any thread. */
    @Volatile private var lastPacket: ByteArray = ByteArray(0)
    /** Mirror of the subscriber addresses for binder-thread CCC reads. */
    @Volatile private var subscribedAddrs: Set<String> = emptySet()

    /** Original adapter name, restored on stop so we don't rename the phone. */
    private var originalAdapterName: String? = null
    private var requestedName: String? = null

    val isAdvertising get() = started
    val connectedCount get() = subscribed.size

    val isBluetoothOn: Boolean get() = adapter?.isEnabled == true
    val isPeripheralCapable: Boolean
        get() = adapter?.isMultipleAdvertisementSupported == true

    // ---- Lifecycle (main thread) -------------------------------------------
    fun start(): Boolean {
        if (started) return true
        val a = adapter ?: run { listener?.onAdvertising(false, "Bluetooth unavailable"); return false }
        if (!a.isEnabled) { listener?.onAdvertising(false, "Bluetooth is off"); return false }
        if (a.bluetoothLeAdvertiser == null) {
            listener?.onAdvertising(false, "This device can't advertise BLE (no peripheral role)")
            return false
        }
        if (!openServer()) {
            listener?.onAdvertising(false, "Could not open a GATT server")
            return false
        }
        started = true
        applyAdvertisedName()
        // Seed the read cache so a read-before-first-push isn't empty.
        lastPacket = model.buildStatusPacket(0)
        startAdvertising()
        startPushLoop()
        return true
    }

    fun stop() {
        started = false
        stopPushLoop()
        // Unconditional: a stop() racing a still-pending advertise start must
        // still cancel the registration, or the radio keeps a zombie advert.
        try { adapter?.bluetoothLeAdvertiser?.stopAdvertising(advertiseCallback) } catch (ignored: Exception) {}
        advertising = false
        subscribed.clear()
        subscribedAddrs = emptySet()
        mtus.clear()
        sendQueue.clear()
        sending = false
        try { server?.close() } catch (ignored: Exception) {}
        server = null
        notifyChar = null
        restoreAdapterName()
    }

    // ---- GATT server -------------------------------------------------------
    private fun openServer(): Boolean {
        // Never stack a second server on a leftover one.
        try { server?.close() } catch (ignored: Exception) {}
        server = null
        val srv = btManager.openGattServer(context, serverCallback) ?: return false
        server = srv

        val service = BluetoothGattService(SERVICE_UUID, BluetoothGattService.SERVICE_TYPE_PRIMARY)
        val notify = BluetoothGattCharacteristic(
            NOTIFY_UUID,
            BluetoothGattCharacteristic.PROPERTY_NOTIFY or BluetoothGattCharacteristic.PROPERTY_READ,
            BluetoothGattCharacteristic.PERMISSION_READ,
        )
        notify.addDescriptor(
            BluetoothGattDescriptor(
                CCC_UUID,
                BluetoothGattDescriptor.PERMISSION_READ or BluetoothGattDescriptor.PERMISSION_WRITE,
            )
        )
        val write = BluetoothGattCharacteristic(
            WRITE_UUID,
            BluetoothGattCharacteristic.PROPERTY_WRITE or
                BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
            BluetoothGattCharacteristic.PERMISSION_WRITE,
        )
        service.addCharacteristic(notify)
        service.addCharacteristic(write)
        srv.addService(service)
        notifyChar = notify
        return true
    }

    // Binder-thread callbacks: copy, respond, and post the real work to main.
    private val serverCallback = object : BluetoothGattServerCallback() {
        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                main.post { listener?.onCentralConnected(device.address) }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                main.post {
                    subscribed.remove(device)
                    subscribedAddrs = subscribed.map { it.address }.toSet()
                    mtus.remove(device.address)
                    sendQueue.removeAll { it.first.address == device.address }
                    listener?.onCentralDisconnected(device.address)
                    // Many stacks stop legacy advertising once a central
                    // connects; re-arm so the next central can find us.
                    if (started && !advertising) startAdvertising()
                }
            }
        }

        override fun onMtuChanged(device: BluetoothDevice, mtu: Int) {
            main.post { mtus[device.address] = mtu }
        }

        override fun onCharacteristicWriteRequest(
            device: BluetoothDevice, requestId: Int, characteristic: BluetoothGattCharacteristic,
            preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray,
        ) {
            val isCommand = characteristic.uuid == WRITE_UUID
            if (responseNeeded) {
                server?.sendResponse(device, requestId,
                    if (isCommand) BluetoothGatt.GATT_SUCCESS else BluetoothGatt.GATT_FAILURE,
                    offset, null)
            }
            if (!isCommand) return
            val cmd = value.copyOf()
            main.post {
                val decoded = model.decode(cmd)
                listener?.onCommand(decoded.description)
                // The real device answers a poll (and reflects any change) with a
                // fresh status frame — push immediately for snappy feedback.
                pushStatus(force = true)
            }
        }

        override fun onDescriptorWriteRequest(
            device: BluetoothDevice, requestId: Int, descriptor: BluetoothGattDescriptor,
            preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray,
        ) {
            if (responseNeeded) {
                server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, null)
            }
            if (descriptor.uuid == CCC_UUID) {
                val enable = value.isNotEmpty() && value[0].toInt() != 0
                main.post {
                    if (enable) subscribed.add(device) else subscribed.remove(device)
                    subscribedAddrs = subscribed.map { it.address }.toSet()
                }
            }
        }

        override fun onDescriptorReadRequest(
            device: BluetoothDevice, requestId: Int, offset: Int, descriptor: BluetoothGattDescriptor,
        ) {
            // Never leave a read unanswered — an unanswered CCC read is an ATT
            // timeout that drops the whole connection.
            if (descriptor.uuid == CCC_UUID) {
                val v = if (device.address in subscribedAddrs)
                    BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                else BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
                server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, v)
            } else {
                server?.sendResponse(device, requestId, BluetoothGatt.GATT_FAILURE, offset, null)
            }
        }

        override fun onCharacteristicReadRequest(
            device: BluetoothDevice, requestId: Int, offset: Int,
            characteristic: BluetoothGattCharacteristic,
        ) {
            if (characteristic.uuid == NOTIFY_UUID) {
                // Serve the cached last frame — no model access off the main thread.
                val pkt = lastPacket
                if (offset > pkt.size) {
                    server?.sendResponse(device, requestId, GATT_INVALID_OFFSET, offset, null)
                } else {
                    server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset,
                        pkt.copyOfRange(offset, pkt.size))
                }
            } else {
                server?.sendResponse(device, requestId, BluetoothGatt.GATT_FAILURE, offset, null)
            }
        }

        override fun onNotificationSent(device: BluetoothDevice, status: Int) {
            main.post { sending = false; sendNext() }
        }
    }

    // ---- Advertising -------------------------------------------------------
    private val advertiseCallback = object : AdvertiseCallback() {
        override fun onStartSuccess(settingsInEffect: AdvertiseSettings?) {
            main.post {
                if (!started) {
                    // stop() raced the pending start — cancel the late advert.
                    try { adapter?.bluetoothLeAdvertiser?.stopAdvertising(this) } catch (ignored: Exception) {}
                    return@post
                }
                advertising = true
                listener?.onAdvertising(true, null)
            }
        }

        override fun onStartFailure(errorCode: Int) {
            main.post {
                advertising = false
                if (!started) return@post
                // Tear down fully so a retry starts clean instead of stacking
                // servers / wedging the toggle.
                stop()
                listener?.onAdvertising(false, "Advertise failed (code $errorCode)")
            }
        }
    }

    private fun startAdvertising() {
        val advertiser = adapter?.bluetoothLeAdvertiser ?: return
        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
            .setConnectable(true)
            .build()
        // The name is carried in the scan response so the 31-byte advert has room
        // for the service UUID (the app/ESP scan lists by name).
        val data = AdvertiseData.Builder()
            .setIncludeDeviceName(false)
            .addServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()
        val scanResponse = AdvertiseData.Builder()
            .setIncludeDeviceName(true)
            .build()
        try {
            advertiser.startAdvertising(settings, data, scanResponse, advertiseCallback)
        } catch (e: Exception) {
            main.post { listener?.onAdvertising(false, "Advertise error: ${e.message}") }
        }
    }

    /**
     * Name to advertise as. The adapter name is a system-wide setting, so the
     * phone's original name is remembered and restored on [stop].
     */
    fun setAdvertisedName(name: String) {
        requestedName = name
        if (started) applyAdvertisedName()
    }

    private fun applyAdvertisedName() {
        val name = requestedName ?: return
        try {
            val a = adapter ?: return
            if (originalAdapterName == null) originalAdapterName = a.name
            if (a.name != name) a.name = name
        } catch (ignored: Exception) {}
    }

    private fun restoreAdapterName() {
        val orig = originalAdapterName ?: return
        try { adapter?.name = orig } catch (ignored: Exception) {}
        originalAdapterName = null
    }

    // ---- Status push loop + paced, MTU-chunked notify queue (main thread) ---
    private var pushRunnable: Runnable? = null
    private var lastPushMs = 0L
    private val sendQueue = ArrayDeque<Pair<BluetoothDevice, ByteArray>>()
    private var sending = false

    private fun startPushLoop() {
        stopPushLoop()
        // Monotonic clock: a wall-clock step (NTP, timezone) must not corrupt
        // the battery-drain integration.
        lastPushMs = SystemClock.elapsedRealtime()
        val r = object : Runnable {
            override fun run() {
                pushStatus(force = false)
                main.postDelayed(this, pushIntervalMs)
            }
        }
        pushRunnable = r
        main.postDelayed(r, pushIntervalMs)
    }

    private fun stopPushLoop() {
        pushRunnable?.let { main.removeCallbacks(it) }
        pushRunnable = null
    }

    private fun pushStatus(force: Boolean) {
        val now = SystemClock.elapsedRealtime()
        val dt = (now - lastPushMs).coerceAtLeast(0)
        // Only advance the physics clock on the timed push; a forced push after a
        // command reuses the same instant so accumulators aren't double-counted.
        val pkt = model.buildStatusPacket(if (force) 0 else dt)
        if (!force) lastPushMs = now
        lastPacket = pkt
        if (notifyChar == null) return
        val targets = subscribed.toList()
        if (targets.isEmpty()) return
        // Bound the queue: if a central stalls, drop the oldest chunks rather
        // than growing forever — the next frame supersedes them anyway.
        while (sendQueue.size > MAX_QUEUED_CHUNKS) sendQueue.removeFirst()
        for (device in targets) {
            val payloadCap = ((mtus[device.address] ?: DEFAULT_MTU) - 3).coerceAtLeast(20)
            var off = 0
            while (off < pkt.size) {
                val end = minOf(off + payloadCap, pkt.size)
                sendQueue.addLast(device to pkt.copyOfRange(off, end))
                off = end
            }
        }
        sendNext()
        listener?.onNotified(model.lastVoltage, model.lastCurrent, model.lastWarning)
    }

    private fun sendNext() {
        if (sending) return
        val notify = notifyChar ?: return
        val srv = server ?: return
        while (sendQueue.isNotEmpty()) {
            val (device, chunk) = sendQueue.removeFirst()
            notify.value = chunk
            val ok = try {
                srv.notifyCharacteristicChanged(device, notify, false)
            } catch (e: Exception) {
                false
            }
            if (ok) { sending = true; return }  // wait for onNotificationSent
            // Failed (device gone / stack busy): drop this chunk and try the next.
        }
    }

    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("0000fff0-0000-1000-8000-00805f9b34fb")
        val NOTIFY_UUID: UUID = UUID.fromString("0000fff1-0000-1000-8000-00805f9b34fb")
        val WRITE_UUID: UUID = UUID.fromString("0000fff3-0000-1000-8000-00805f9b34fb")
        val CCC_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        private const val DEFAULT_MTU = 23
        private const val GATT_INVALID_OFFSET = 0x07
        private const val MAX_QUEUED_CHUNKS = 64
    }
}
