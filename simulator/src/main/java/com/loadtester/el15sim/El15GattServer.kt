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
import java.util.UUID

/**
 * Advertises the EL15's FFF0 service and runs a GATT server that behaves like
 * the real load: it accepts command writes on FFF3, and pushes 28-byte status
 * notifications on FFF1 (on every poll, on any state change, and on a steady
 * timer so the load's accumulators keep advancing while the ESP is connected).
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
    private var advertising = false
    private var lastPushMs = 0L

    val isAdvertising get() = advertising
    val connectedCount get() = subscribed.size

    val isBluetoothOn: Boolean get() = adapter?.isEnabled == true
    val isPeripheralCapable: Boolean
        get() = adapter?.isMultipleAdvertisementSupported == true

    // ---- Lifecycle ---------------------------------------------------------
    fun start(): Boolean {
        if (advertising) return true
        val a = adapter ?: run { listener?.onAdvertising(false, "Bluetooth unavailable"); return false }
        if (!a.isEnabled) { listener?.onAdvertising(false, "Bluetooth is off"); return false }
        if (a.bluetoothLeAdvertiser == null) {
            listener?.onAdvertising(false, "This device can't advertise BLE (no peripheral role)")
            return false
        }
        a.name?.let { /* advertised name comes from the adapter name */ }
        openServer()
        startAdvertising()
        startPushLoop()
        return true
    }

    fun stop() {
        stopPushLoop()
        stopAdvertising()
        subscribed.clear()
        try { server?.close() } catch (ignored: Exception) {}
        server = null
        notifyChar = null
    }

    // ---- GATT server -------------------------------------------------------
    private fun openServer() {
        val srv = btManager.openGattServer(context, serverCallback) ?: return
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
    }

    private val serverCallback = object : BluetoothGattServerCallback() {
        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                main.post { listener?.onCentralConnected(device.address) }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                subscribed.remove(device)
                main.post { listener?.onCentralDisconnected(device.address) }
            }
        }

        override fun onCharacteristicWriteRequest(
            device: BluetoothDevice, requestId: Int, characteristic: BluetoothGattCharacteristic,
            preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray,
        ) {
            if (characteristic.uuid == WRITE_UUID) {
                val decoded = model.decode(value)
                main.post { listener?.onCommand(decoded.description) }
                if (responseNeeded) {
                    server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, null)
                }
                // The real device answers a poll (and reflects any change) with a
                // fresh status frame — push immediately for snappy feedback.
                pushStatus(force = true)
            } else if (responseNeeded) {
                server?.sendResponse(device, requestId, BluetoothGatt.GATT_FAILURE, offset, null)
            }
        }

        override fun onDescriptorWriteRequest(
            device: BluetoothDevice, requestId: Int, descriptor: BluetoothGattDescriptor,
            preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray,
        ) {
            if (descriptor.uuid == CCC_UUID) {
                val enable = value.isNotEmpty() && value[0].toInt() != 0
                if (enable) subscribed.add(device) else subscribed.remove(device)
            }
            if (responseNeeded) {
                server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, null)
            }
        }

        override fun onCharacteristicReadRequest(
            device: BluetoothDevice, requestId: Int, offset: Int,
            characteristic: BluetoothGattCharacteristic,
        ) {
            if (characteristic.uuid == NOTIFY_UUID) {
                val pkt = model.buildStatusPacket(0)
                val slice = if (offset in 0..pkt.size) pkt.copyOfRange(offset, pkt.size) else ByteArray(0)
                server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, slice)
            } else {
                server?.sendResponse(device, requestId, BluetoothGatt.GATT_FAILURE, offset, null)
            }
        }
    }

    // ---- Advertising -------------------------------------------------------
    private val advertiseCallback = object : AdvertiseCallback() {
        override fun onStartSuccess(settingsInEffect: AdvertiseSettings?) {
            advertising = true
            main.post { listener?.onAdvertising(true, null) }
        }

        override fun onStartFailure(errorCode: Int) {
            advertising = false
            main.post { listener?.onAdvertising(false, "Advertise failed (code $errorCode)") }
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
            listener?.onAdvertising(false, "Advertise error: ${e.message}")
        }
    }

    private fun stopAdvertising() {
        if (!advertising) return
        try { adapter?.bluetoothLeAdvertiser?.stopAdvertising(advertiseCallback) } catch (ignored: Exception) {}
        advertising = false
    }

    /** Rename the advertised peripheral (adapter name; affects all BT). */
    fun setAdvertisedName(name: String) {
        try { adapter?.name = name } catch (ignored: Exception) {}
    }

    // ---- Status push loop --------------------------------------------------
    private var pushRunnable: Runnable? = null

    private fun startPushLoop() {
        stopPushLoop()
        lastPushMs = System.currentTimeMillis()
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
        val now = System.currentTimeMillis()
        val dt = (now - lastPushMs).coerceAtLeast(0)
        // Only advance the physics clock on the timed push; a forced push after a
        // command reuses the same instant so accumulators aren't double-counted.
        val pkt = model.buildStatusPacket(if (force) 0 else dt)
        if (!force) lastPushMs = now
        val notify = notifyChar ?: return
        notify.value = pkt
        val targets = subscribed.toList()
        for (device in targets) {
            try { server?.notifyCharacteristicChanged(device, notify, false) } catch (ignored: Exception) {}
        }
        if (targets.isNotEmpty()) {
            listener?.onNotified(model.lastVoltage, model.lastCurrent, model.lastWarning)
        }
    }

    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("0000fff0-0000-1000-8000-00805f9b34fb")
        val NOTIFY_UUID: UUID = UUID.fromString("0000fff1-0000-1000-8000-00805f9b34fb")
        val WRITE_UUID: UUID = UUID.fromString("0000fff3-0000-1000-8000-00805f9b34fb")
        val CCC_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
}
