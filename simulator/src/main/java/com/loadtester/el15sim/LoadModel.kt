package com.loadtester.el15sim

import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

/**
 * The simulated EL15 load, from the *device's* point of view.
 *
 * The phone/ESP app is the BLE central: it writes command frames and parses
 * status packets. This simulator is the peripheral, so it does the inverse —
 * it **decodes** command frames and **encodes** 28-byte status packets — using
 * the exact same wire format (reverse-engineered from DM40GUI) so a real BLE
 * central can't tell it from hardware.
 *
 * The load models a virtual circuit under test: an ideal source of [emf] volts
 * behind [seriesR] ohms, honouring mode/setpoint/load commands and enforcing
 * the EL15's 150 W / 60 V / 12 A ratings (over-power/over-current trip).
 */
class LoadModel {

    // Virtual circuit — editable live from the UI.
    var emf: Float = 12.6f
    var seriesR: Float = 0.35f

    // Commanded state (set by decoded writes).
    var mode: Int = MODE_CC
        private set
    var setpoint: Float = 0f
        private set
    var loadOn: Boolean = false
        private set
    var lockOn: Boolean = false
        private set

    // Last solved outputs, for the UI.
    var lastVoltage: Float = 0f; private set
    var lastCurrent: Float = 0f; private set
    var lastWarning: String = ""; private set

    private var energyWh = 0f
    private var capacityAh = 0f
    private var runtimeSec = 0
    private var runMsAcc = 0L
    private var rngState = 0x2545F491.toInt()

    private val rEff: Float get() = max(seriesR, 0.001f)

    /** Result of decoding one command frame. */
    data class Decoded(val description: String, val known: Boolean)

    /** Decode a command frame written to FFF3 and update state. */
    fun decode(cmd: ByteArray): Decoded {
        if (cmd.size < 4 || (cmd[0].toInt() and 0xFF) != 0xAF) {
            return Decoded("Unknown frame (${hex(cmd)})", false)
        }
        return when (cmd[3].toInt() and 0xFF) {
            0x08 -> Decoded("POLL", true)
            0x09 -> {
                when (cmd.getOrNull(5)?.toInt()?.and(0xFF)) {
                    0x04 -> { loadOn = true; Decoded("LOAD ON", true) }
                    0x00 -> { loadOn = false; runtimeSec = 0; runMsAcc = 0; Decoded("LOAD OFF", true) }
                    0x01 -> { lockOn = !lockOn; Decoded("LOCK → ${if (lockOn) "on" else "off"}", true) }
                    else -> Decoded("Action ${hex(cmd)}", false)
                }
            }
            0x03 -> {
                val m = cmd.getOrNull(5)?.toInt()?.and(0xFF) ?: return Decoded("MODE (short)", false)
                mode = m
                energyWh = 0f; capacityAh = 0f; runtimeSec = 0; runMsAcc = 0
                Decoded("MODE → ${MODE_NAMES[m] ?: "?%02X".format(m)}", true)
            }
            0x04 -> {
                if (cmd.size < 9) return Decoded("SETPOINT (short)", false)
                setpoint = max(f32(cmd, 5), 0f)
                Decoded("SETPOINT → %.3f".format(setpoint), true)
            }
            else -> Decoded("Unknown op 0x%02X".format(cmd[3].toInt() and 0xFF), false)
        }
    }

    /**
     * Advance the physics by [dtMs] and return the 28-byte status packet the
     * central expects on FFF1. Inverse of El15Protocol.parseStatus.
     */
    fun buildStatusPacket(dtMs: Long): ByteArray {
        val (voltage, current, warn) = solveClamped()
        lastVoltage = voltage; lastCurrent = current; lastWarning = warn
        if (loadOn && current > 0.001f) {
            runMsAcc += dtMs
            runtimeSec = (runMsAcc / 1000L).toInt()
            val dtH = dtMs / 3_600_000f
            energyWh += voltage * current * dtH
            capacityAh += current * dtH
        }
        return encodePacket(voltage, current, warn)
    }

    fun reset() {
        energyWh = 0f; capacityAh = 0f; runtimeSec = 0; runMsAcc = 0
    }

    // ---- Circuit solve (ported from the app's El15Simulator) ----------------
    private fun solveClamped(): Triple<Float, Float, String> {
        val (vRaw, iRaw) = solve()
        var i = min(iRaw, MAX_CURRENT_A)
        var v = if (loadOn) max(emf - i * rEff, 0f) else vRaw
        var warn = ""
        if (loadOn && v * i > MAX_POWER_W * 1.02f) {
            val disc = emf * emf - 4 * rEff * MAX_POWER_W
            i = if (disc > 0f) (emf - sqrt(disc)) / (2 * rEff) else MAX_POWER_W / max(v, 0.1f)
            v = max(emf - i * rEff, 0f)
            warn = "OPP"
        } else if (loadOn && iRaw > MAX_CURRENT_A * 1.001f) {
            warn = "OCP"
        }
        return Triple(v.withNoise(), i.withNoise(), warn)
    }

    private fun solve(): Pair<Float, Float> {
        if (!loadOn) return emf to 0f
        val iCeil = (emf - 0.3f) / rEff
        val i = when (mode) {
            MODE_CC, MODE_CAP, MODE_DCR -> min(setpoint, iCeil)
            MODE_CV -> max((emf - min(setpoint, emf)) / rEff, 0f)
            MODE_CR -> emf / (rEff + max(setpoint, 0.01f))
            MODE_CP -> {
                val disc = emf * emf - 4 * rEff * setpoint
                if (disc <= 0f) iCeil else min((emf - sqrt(disc)) / (2 * rEff), iCeil)
            }
            else -> min(setpoint, iCeil)
        }
        val current = max(i, 0f)
        return max(emf - current * rEff, 0f) to current
    }

    // ---- Packet encoding (inverse of parseStatus) ---------------------------
    private fun fanFor(current: Float): Int = when {
        current > 8f -> FAN_SPEED_MAX
        current > 4f -> 3
        current > 1f -> 1
        else -> 0
    }

    private fun encodePacket(voltage: Float, current: Float, warn: String): ByteArray {
        val pkt = ByteArray(28)
        pkt[0] = 0xDF.toByte(); pkt[1] = 0x07; pkt[2] = 0x03; pkt[3] = 0x08
        val fan = fanFor(current)
        // A protection state sets the warn flag (bit pattern 0x06) in byte5 and
        // the code in the high nibble of byte6, matching parseStatus.
        val warnBits = if (warn.isNotEmpty()) 0x06 else 0x00
        pkt[5] = (((mode and 0x1F) or warnBits) or ((fan and 0x03) shl 6)).toByte()
        val warnCode = when (warn) { "REV" -> 0x6; "UVP" -> 0x9; "OPP", "OCP" -> 0xE; else -> 0x0 }
        pkt[6] = (((if (loadOn) 0x02 else 0)) or (if (lockOn) 0x04 else 0) or
            ((fan shr 2) and 0x01) or (warnCode shl 4)).toByte()
        putF32(pkt, 7, voltage)
        putF32(pkt, 11, current)
        when (mode) {
            MODE_CAP -> {
                putI32(pkt, 15, runtimeSec)
                putF32(pkt, 19, energyWh * 1000f)
                putF32(pkt, 23, capacityAh * 1000f)
            }
            MODE_DCR -> {
                putF32(pkt, 15, current)
                putF32(pkt, 19, current * 0.5f)
                putF32(pkt, 23, rEff * 1000f)
            }
            else -> {
                putI32(pkt, 15, runtimeSec)
                putF32(pkt, 19, 24.5f + current * 0.9f) // temperature
                putF32(pkt, 23, setpoint)
            }
        }
        var sum = 0
        for (k in 0..26) sum += pkt[k].toInt() and 0xFF
        pkt[27] = ((-sum) and 0xFF).toByte()
        return pkt
    }

    private fun putF32(a: ByteArray, off: Int, v: Float) {
        val bits = java.lang.Float.floatToIntBits(v)
        a[off] = bits.toByte()
        a[off + 1] = (bits ushr 8).toByte()
        a[off + 2] = (bits ushr 16).toByte()
        a[off + 3] = (bits ushr 24).toByte()
    }

    private fun putI32(a: ByteArray, off: Int, v: Int) {
        a[off] = v.toByte()
        a[off + 1] = (v ushr 8).toByte()
        a[off + 2] = (v ushr 16).toByte()
        a[off + 3] = (v ushr 24).toByte()
    }

    private fun f32(a: ByteArray, off: Int): Float {
        val bits = (a[off].toInt() and 0xFF) or ((a[off + 1].toInt() and 0xFF) shl 8) or
            ((a[off + 2].toInt() and 0xFF) shl 16) or ((a[off + 3].toInt() and 0xFF) shl 24)
        return java.lang.Float.intBitsToFloat(bits)
    }

    private fun Float.withNoise(): Float {
        rngState = rngState xor (rngState shl 13)
        rngState = rngState xor (rngState ushr 17)
        rngState = rngState xor (rngState shl 5)
        val unit = (abs(rngState % 1000) / 1000f) * 2f - 1f
        return this * (1f + unit * 0.0015f)
    }

    private fun hex(b: ByteArray): String = b.joinToString(" ") { "%02X".format(it) }

    companion object {
        const val MODE_CC = 0x01
        const val MODE_CAP = 0x02
        const val MODE_CV = 0x09
        const val MODE_DCR = 0x0A
        const val MODE_CR = 0x11
        const val MODE_CP = 0x19

        const val MAX_CURRENT_A = 12f
        const val MAX_POWER_W = 150f
        const val FAN_SPEED_MAX = 5

        val MODE_NAMES: Map<Int, String> = mapOf(
            MODE_CC to "CC", MODE_CAP to "CAP", MODE_CV to "CV",
            MODE_DCR to "DCR", MODE_CR to "CR", MODE_CP to "CP",
        )
    }
}
