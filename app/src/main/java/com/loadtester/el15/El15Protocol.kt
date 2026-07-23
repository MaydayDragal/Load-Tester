package com.loadtester.el15

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

/**
 * EL15 electronic-load BLE protocol.
 *
 * Ported from the DM40GUI reference implementation
 * (github.com/maj113/DM40GUI, el15/protocol_constants.py).
 *
 * The device exposes a Nordic-style FFF0 service. Status packets are pushed as
 * notifications on FFF1; command frames are written to FFF3.
 */
object El15Protocol {

    // ---- GATT UUIDs -------------------------------------------------------
    val SERVICE_UUID: UUID = UUID.fromString("0000fff0-0000-1000-8000-00805f9b34fb")
    val NOTIFY_UUID: UUID = UUID.fromString("0000fff1-0000-1000-8000-00805f9b34fb")
    val WRITE_UUID: UUID = UUID.fromString("0000fff3-0000-1000-8000-00805f9b34fb")

    /** Standard Client Characteristic Configuration descriptor. */
    val CCC_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    // ---- Fixed command frames (captured, replayed verbatim) ---------------
    /** Poll for a fresh status packet. Prefix + CRC byte 0x3F. */
    val POLL: ByteArray = byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x08, 0x00, 0x3F)
    val LOAD_ON: ByteArray = byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x09, 0x01, 0x04)
    val LOAD_OFF: ByteArray = byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x09, 0x01, 0x00)
    val LOCK: ByteArray = byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x09, 0x01, 0x01)

    private val MODE_PREFIX: ByteArray = byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x03, 0x01)
    private val SETPOINT_PREFIX: ByteArray = byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x04, 0x04)

    private val HEADER: ByteArray = byteArrayOf(0xDF.toByte(), 0x07, 0x03, 0x08)

    // ---- Modes ------------------------------------------------------------
    const val MODE_CC = 0x01
    const val MODE_CAP = 0x02
    const val MODE_DT = 0x03
    const val MODE_ADV = 0x04
    const val MODE_CV = 0x09
    const val MODE_DCR = 0x0A
    const val MODE_POWER = 0x0B
    const val MODE_ADV_SCAN = 0x0C
    const val MODE_POWER_RPT = 0x0D
    const val MODE_CR = 0x11
    const val MODE_CP = 0x19

    val MODE_NAMES: Map<Int, String> = mapOf(
        MODE_CC to "CC",
        MODE_CAP to "CAP",
        MODE_DT to "POW [DT]",
        MODE_ADV to "ADV [L]",
        MODE_CV to "CV",
        MODE_DCR to "DCR",
        MODE_POWER to "POW [A]",
        MODE_ADV_SCAN to "ADV [S]",
        MODE_POWER_RPT to "POW [RPT]",
        MODE_CR to "CR",
        MODE_CP to "CP",
    )

    /** (unit, decimal places, label) for the settable modes. */
    data class SetpointInfo(val unit: String, val decimals: Int, val label: String)

    val MODE_SETPOINT_INFO: Map<Int, SetpointInfo> = mapOf(
        MODE_CC to SetpointInfo("A", 3, "Current"),
        MODE_CAP to SetpointInfo("A", 3, "Current"),
        MODE_CV to SetpointInfo("V", 3, "Voltage"),
        MODE_DCR to SetpointInfo("A", 3, "Current"),
        MODE_CR to SetpointInfo("Ω", 1, "Resistance"),
        MODE_CP to SetpointInfo("W", 2, "Power"),
        // ADV-family modes have no user-facing setpoint; blank the fields to
        // match the DM40GUI reference (they're reachable only from the device's
        // own front panel). Otherwise the readout would show "Setpoint (?)".
        MODE_ADV to SetpointInfo("", 3, ""),
        MODE_DT to SetpointInfo("", 3, ""),
        MODE_ADV_SCAN to SetpointInfo("", 3, ""),
        MODE_POWER to SetpointInfo("", 3, ""),
        MODE_POWER_RPT to SetpointInfo("", 3, ""),
    )

    /** Modes the user can pick from the UI (those with a meaningful setpoint). */
    val SELECTABLE_MODES: List<Int> =
        listOf(MODE_CC, MODE_CV, MODE_CR, MODE_CP, MODE_CAP, MODE_DCR)

    // ---- Command builders -------------------------------------------------
    fun modeCommand(mode: Int): ByteArray = MODE_PREFIX + byteArrayOf(mode.toByte())

    /** value encoded as little-endian IEEE-754 float32. */
    fun setpointCommand(value: Float): ByteArray {
        val f = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putFloat(value).array()
        return SETPOINT_PREFIX + f
    }

    // ---- Status packet parsing -------------------------------------------
    private const val B5_WARN_FLAG = 0x06
    private const val MODE_MASK = 0x1F
    private const val STATUS_LOAD_BIT = 0x02
    private const val STATUS_LOCK_BIT = 0x04

    private val WARN_NAMES = mapOf(0x6 to "REV", 0x9 to "UVP")

    const val FAN_SPEED_MAX = 5

    // ---- Hardware ratings (ALIENTEK EL15: 150 W / 60 V / 12 A) -------------
    // Auto tests must never command beyond these. Power is usually the binding
    // limit: at a high source voltage, 150 W allows far less than 12 A.
    const val MAX_CURRENT_A = 12f
    const val MAX_POWER_W = 150f
    const val MAX_VOLTAGE_V = 60f
    const val MIN_VOLTAGE_V = 0.1f

    /** True if [data] begins with the EL15 status header. */
    fun isStatusPacket(data: ByteArray): Boolean =
        data.size >= 4 && data.copyOfRange(0, 4).contentEquals(HEADER)

    private fun f32(data: ByteArray, off: Int): Float =
        ByteBuffer.wrap(data, off, 4).order(ByteOrder.LITTLE_ENDIAN).float

    private fun i32(data: ByteArray, off: Int): Int =
        ByteBuffer.wrap(data, off, 4).order(ByteOrder.LITTLE_ENDIAN).int

    /** Parse a 28-byte EL15 status notification. */
    fun parseStatus(data: ByteArray): El15Status {
        val s = El15Status()
        s.raw = data.joinToString(" ") { "%02X".format(it) }
        s.crcPass = (data.sumOf { it.toInt() and 0xFF } and 0xFF) == 0
        // Corrupt frames stay valid=false so nothing downstream consumes their
        // values; raw/crcPass remain populated for the packet inspector.
        if (data.size < 28 || !isStatusPacket(data) || !s.crcPass) {
            return s
        }

        s.voltage = f32(data, 7)
        s.current = f32(data, 11)
        var runtime = i32(data, 15)
        s.power = s.voltage * s.current

        val b5 = data[5].toInt() and 0xFF
        val b6 = data[6].toInt() and 0xFF

        val warnFlag = (b5 and B5_WARN_FLAG) == B5_WARN_FLAG
        val rawMode = if (warnFlag) {
            b5 and (MODE_MASK and B5_WARN_FLAG.inv())
        } else {
            b5 and MODE_MASK
        }
        val mode = if (MODE_NAMES.containsKey(rawMode)) rawMode else (rawMode or 0x01)
        s.mode = mode

        if (warnFlag) {
            val warnCode = b6 shr 4
            s.warning = WARN_NAMES[warnCode] ?: "PROT %X".format(warnCode)
            s.ready = false
        } else {
            s.ready = (rawMode and 0x01) != 0 || mode in setOf(
                MODE_CAP, MODE_DCR, MODE_ADV, MODE_POWER, MODE_DT, MODE_ADV_SCAN, MODE_POWER_RPT
            )
        }

        when (mode) {
            MODE_CAP -> {
                s.energyWh = f32(data, 19) * 0.001f
                s.capacityAh = f32(data, 23) * 0.001f
                s.setpointInPacket = false
            }
            MODE_DCR -> {
                s.dcrI1 = f32(data, 15)
                s.dcrI2 = f32(data, 19)
                s.dcrMilliOhm = f32(data, 23)
                runtime = 0
                s.current = 0f
                s.power = 0f
                s.setpointInPacket = false
            }
            MODE_ADV, MODE_POWER, MODE_DT, MODE_POWER_RPT -> {
                runtime = 0
                s.setpointInPacket = false
            }
            else -> {
                s.temperature = f32(data, 19)
                s.setpoint = f32(data, 23)
            }
        }
        s.runtime = runtime

        // Fan speed spans two bytes: byte5 bits 6-7 -> low bits, byte6 bit0 -> MSB.
        s.fanSpeed = (b5 shr 6) or ((b6 and 0x01) shl 2)
        s.loadOn = (b6 and STATUS_LOAD_BIT) != 0
        s.lockOn = (b6 and STATUS_LOCK_BIT) != 0
        s.modeName = MODE_NAMES[mode] ?: "?%02X".format(mode)

        val info = MODE_SETPOINT_INFO[mode] ?: SetpointInfo("?", 3, "Setpoint")
        s.setpointUnit = info.unit
        s.setpointDecimals = info.decimals
        s.setpointLabel = info.label

        s.valid = true
        return s
    }
}

/** Decoded EL15 status snapshot. Mirrors the reference EL15Status class. */
class El15Status {
    var raw: String = ""
    var crcPass: Boolean = false
    var valid: Boolean = false

    var voltage: Float = 0f
    var current: Float = 0f
    var power: Float = 0f
    var runtime: Int = 0
    var temperature: Float = 0f
    var setpoint: Float = 0f

    var energyWh: Float = 0f
    var capacityAh: Float = 0f

    var dcrMilliOhm: Float = 0f
    var dcrI1: Float = 0f
    var dcrI2: Float = 0f

    var mode: Int = El15Protocol.MODE_CC
    var modeName: String = "---"
    var fanSpeed: Int = 0
    var loadOn: Boolean = false
    var lockOn: Boolean = false
    var ready: Boolean = false

    var setpointUnit: String = "A"
    var setpointDecimals: Int = 3
    var setpointLabel: String = "Current"
    var setpointInPacket: Boolean = true
    var warning: String = ""
}
