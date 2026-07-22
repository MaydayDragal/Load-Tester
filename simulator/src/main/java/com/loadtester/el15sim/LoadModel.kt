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
 * Two selectable sources sit behind the load's terminals:
 *
 *  - **Fixed circuit** — an ideal source of [emf] volts behind [seriesR] ohms
 *    (never depletes; a resistance test recovers exactly these values).
 *  - **Battery** — a chemistry-accurate pack: open-circuit voltage follows a
 *    per-chemistry OCV-vs-state-of-charge lookup curve (plateau + knee) scaled
 *    by [cells]; the terminal sags by I × [batteryR]; charge drawn is
 *    coulomb-counted against [capacityAh], so a capacity test run from the
 *    central reproduces the full discharge curve down to its cutoff.
 *
 * Either way the EL15's 150 W / 60 V / 12 A ratings are enforced
 * (over-power/over-current trip frames, like the real unit).
 */
class LoadModel {

    // ---- Source selection ---------------------------------------------------
    /** false = fixed circuit (emf/seriesR); true = battery pack. */
    var batteryMode: Boolean = false

    // Fixed circuit — editable live from the UI.
    var emf: Float = 12.6f
    var seriesR: Float = 0.35f

    // Battery pack — editable live from the UI. (batteryCapacityAh, not
    // capacityAh: that name is taken by the CAP-mode telemetry accumulator.)
    var chemistry: Int = CHEM_LIION
    var cells: Int = 3
    var batteryCapacityAh: Float = 2.5f
    /** Internal resistance of the whole pack, in ohms. */
    var batteryR: Float = 0.05f

    /** State of charge, 0..1. Depletes as the central sinks current. */
    var soc: Double = 1.0
        private set
    /** Total charge drawn since the last recharge, in Ah. */
    var drawnAh: Double = 0.0
        private set
    /** Charge drawn past empty, in Ah — drives the dead-pack voltage collapse. */
    private var overdrawnAh: Double = 0.0

    /** Reset the battery to the given state of charge (default full). */
    fun recharge(socPct: Float = 100f) {
        soc = (socPct / 100f).coerceIn(0f, 1f).toDouble()
        drawnAh = 0.0
        overdrawnAh = 0.0
    }

    /**
     * Pack open-circuit voltage at the current state of charge. Once the pack
     * is empty, continued drain collapses the voltage toward zero (over ~10 %
     * of capacity of overdraw) — a dead battery can't source charge forever,
     * and this guarantees any discharge cutoff is eventually crossed even if
     * it was set below the chemistry curve's 0 % floor.
     */
    fun ocvNow(): Float {
        val base = cells * ocvPerCell(chemistry, soc.toFloat())
        if (soc > 0.0 || overdrawnAh <= 0.0) return base
        val collapse = (1.0 - overdrawnAh / (0.10 * capacityAhSafe)).coerceIn(0.0, 1.0)
        return (base * collapse).toFloat()
    }

    /** The source EMF the circuit solver sees this tick. */
    private fun sourceV(): Float = if (batteryMode) ocvNow() else emf

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

    private val rEff: Float get() = max(if (batteryMode) batteryR else seriesR, 0.001f)

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
            if (batteryMode && dtMs > 0) {
                // Coulomb counting: deplete state of charge by the charge drawn;
                // drain past empty accumulates as overdraw (voltage collapse).
                val dAh = current * dtH.toDouble()
                drawnAh += dAh
                val newSoc = soc - dAh / capacityAhSafe
                if (newSoc < 0.0) overdrawnAh += -newSoc * capacityAhSafe
                soc = newSoc.coerceAtLeast(0.0)
            }
        }
        return encodePacket(voltage, current, warn)
    }

    private val capacityAhSafe: Float get() = max(batteryCapacityAh, 0.001f)

    fun reset() {
        energyWh = 0f; capacityAh = 0f; runtimeSec = 0; runMsAcc = 0
    }

    // ---- Circuit solve (source = fixed emf or battery OCV at current SoC) ---
    private fun solveClamped(): Triple<Float, Float, String> {
        val e = sourceV()
        val (vRaw, iRaw) = solve(e)
        var i = min(iRaw, MAX_CURRENT_A)
        var v = if (loadOn) max(e - i * rEff, 0f) else vRaw
        var warn = ""
        if (loadOn && v * i > MAX_POWER_W * 1.02f) {
            val disc = e * e - 4 * rEff * MAX_POWER_W
            i = if (disc > 0f) (e - sqrt(disc)) / (2 * rEff) else MAX_POWER_W / max(v, 0.1f)
            v = max(e - i * rEff, 0f)
            warn = "OPP"
        } else if (loadOn && iRaw > MAX_CURRENT_A * 1.001f) {
            warn = "OCP"
        }
        return Triple(v.withNoise(), i.withNoise(), warn)
    }

    private fun solve(e: Float): Pair<Float, Float> {
        if (!loadOn) return e to 0f
        // Physical ceiling: the terminal can't be pulled below ~0.3 V.
        val iCeil = max((e - 0.3f) / rEff, 0f)
        val i = when (mode) {
            MODE_CC, MODE_CAP, MODE_DCR -> min(setpoint, iCeil)
            MODE_CV -> max((e - min(setpoint, e)) / rEff, 0f)
            MODE_CR -> e / (rEff + max(setpoint, 0.01f))
            MODE_CP -> {
                val disc = e * e - 4 * rEff * setpoint
                if (disc <= 0f) iCeil else min((e - sqrt(disc)) / (2 * rEff), iCeil)
            }
            else -> min(setpoint, iCeil)
        }
        val current = max(i, 0f)
        return max(e - current * rEff, 0f) to current
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

        // ---- Battery chemistries -------------------------------------------
        const val CHEM_LIION = 0
        const val CHEM_LIFEPO4 = 1
        const val CHEM_LEAD = 2
        const val CHEM_NIMH = 3

        val CHEM_NAMES = listOf("Li-ion (3.7 V)", "LiFePO₄ (3.2 V)",
            "Lead-acid (2.0 V)", "NiMH (1.2 V)")

        /** Nominal per-cell voltage, used by the UI to suggest a cell count. */
        val CHEM_NOMINAL = floatArrayOf(3.7f, 3.2f, 2.0f, 1.2f)

        /**
         * Per-cell open-circuit voltage vs state of charge, one 21-point table
         * per chemistry (index 0 = 0 % SoC … index 20 = 100 %, 5 % steps),
         * shaped from typical published resting-voltage discharge data: the
         * charge-top drop, the nominal plateau, and the empty-end knee.
         * Terminal voltage under load = interp(curve) × cells − I × batteryR.
         */
        private val OCV_CURVES: Array<FloatArray> = arrayOf(
            // Li-ion (LCO/NMC): 4.20 full → 3.7-3.9 plateau → knee below 10 %.
            floatArrayOf(
                3.00f, 3.50f, 3.68f, 3.71f, 3.73f, 3.75f, 3.77f, 3.79f, 3.80f,
                3.82f, 3.84f, 3.85f, 3.87f, 3.91f, 3.95f, 3.98f, 4.02f, 4.06f,
                4.11f, 4.15f, 4.20f,
            ),
            // LiFePO4: famously flat 3.3 V plateau with sharp knees both ends.
            floatArrayOf(
                2.50f, 3.05f, 3.20f, 3.22f, 3.24f, 3.25f, 3.26f, 3.26f, 3.27f,
                3.27f, 3.28f, 3.28f, 3.29f, 3.29f, 3.30f, 3.30f, 3.31f, 3.32f,
                3.33f, 3.35f, 3.45f,
            ),
            // Lead-acid: near-linear 2.12 → 1.93, collapsing tail when deeply
            // drained. The 0 % floor sits BELOW the conventional 1.75 V/cell
            // discharge cutoff so a capacity test always reaches its endpoint
            // (a floor above the cutoff would leave the test running forever).
            floatArrayOf(
                1.70f, 1.85f, 1.93f, 1.94f, 1.95f, 1.96f, 1.97f, 1.98f, 1.99f,
                2.00f, 2.01f, 2.02f, 2.03f, 2.04f, 2.05f, 2.06f, 2.07f, 2.08f,
                2.09f, 2.105f, 2.12f,
            ),
            // NiMH: 1.40 fresh-off-charge bump, long 1.25 plateau, end knee.
            floatArrayOf(
                1.00f, 1.10f, 1.15f, 1.18f, 1.20f, 1.21f, 1.22f, 1.23f, 1.24f,
                1.24f, 1.25f, 1.25f, 1.26f, 1.26f, 1.27f, 1.28f, 1.29f, 1.30f,
                1.32f, 1.35f, 1.40f,
            ),
        )

        /** Linear interpolation over the chemistry's OCV table. [soc] is 0..1. */
        fun ocvPerCell(chem: Int, soc: Float): Float {
            val curve = OCV_CURVES.getOrElse(chem) { OCV_CURVES[CHEM_LIION] }
            val s = soc.coerceIn(0f, 1f)
            val pos = s * (curve.size - 1)
            val lo = pos.toInt().coerceAtMost(curve.size - 2)
            val frac = pos - lo
            return curve[lo] + (curve[lo + 1] - curve[lo]) * frac
        }
    }
}
