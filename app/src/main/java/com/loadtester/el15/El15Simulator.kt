package com.loadtester.el15

import android.os.Handler
import android.os.Looper
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

/**
 * A fake EL15 "demo device" for use without hardware.
 *
 * Models a virtual circuit under test — an ideal source of [emf] volts behind a
 * series resistance [seriesR] ohms — and behaves like a real load: it honours
 * mode/setpoint/load commands and pushes [El15Status] updates at the poll rate,
 * so the whole app (live readouts, manual controls, and the circuit-resistance
 * test) can be exercised end-to-end. A resistance test against it recovers
 * ~[seriesR] Ω and ~[emf] V.
 *
 * Readings carry a little pseudo-random noise (deterministic, seeded) so charts
 * look realistic without depending on a real RNG.
 */
class El15Simulator(
    private val onStatus: (El15Status) -> Unit,
    private val emf: Float = 12.60f,
    private val seriesR: Float = 0.35f,
) : El15Controller {

    private val main = Handler(Looper.getMainLooper())

    private var mode: Int = El15Protocol.MODE_CC
    private var setpoint: Float = 0f
    private var loadOn: Boolean = false
    private var lockOn: Boolean = false
    private var runtimeSec: Int = 0
    private var ticks: Int = 0
    private var rngState: Int = 0x2545F491

    // Accumulators for CAP mode.
    private var energyWh: Float = 0f
    private var capacityAh: Float = 0f

    private var poll: Runnable? = null

    fun start() {
        stop()
        runtimeSec = 0; ticks = 0; energyWh = 0f; capacityAh = 0f
        val r = object : Runnable {
            override fun run() {
                tick()
                main.postDelayed(this, POLL_MS)
            }
        }
        poll = r
        main.post(r)
    }

    fun stop() {
        poll?.let { main.removeCallbacks(it) }
        poll = null
    }

    // ---- El15Controller ---------------------------------------------------
    override fun setMode(mode: Int) {
        this.mode = mode
        // Reset accumulators when entering CAP/leaving, mirroring device behaviour.
        energyWh = 0f; capacityAh = 0f; runtimeSec = 0
    }

    override fun setSetpoint(value: Float) { setpoint = max(value, 0f) }

    override fun setLoad(on: Boolean) {
        loadOn = on
        if (!on) runtimeSec = 0
    }

    override fun setLock() { lockOn = !lockOn }

    // ---- Simulation core --------------------------------------------------
    private fun tick() {
        ticks++
        val (voltage, current, warn) = solveClamped()
        if (loadOn && current > 0.001f) {
            runtimeSec += (POLL_MS / 1000.0).toInt().coerceAtLeast(0)
            if (ticks % 2 == 0) runtimeSec += 1 // ~1s per two 500ms ticks
            val dtHours = POLL_MS / 3_600_000f
            energyWh += voltage * current * dtHours
            capacityAh += current * dtHours
        }
        onStatus(buildStatus(voltage, current, warn))
    }

    /**
     * Returns (terminalVoltage, current, warning) for the active mode, after
     * enforcing the EL15's hardware limits (12 A max, 150 W over-power). A well-
     * behaved auto test never reaches these; a manual overload trips protection,
     * just like the real unit.
     */
    private fun solveClamped(): Triple<Float, Float, String> {
        val (vRaw, iRaw) = solve()
        var i = min(iRaw, El15Protocol.MAX_CURRENT_A)
        var v = if (loadOn) max(emf - i * seriesR, 0f) else vRaw
        var warn = ""
        // Over-power protection (with a small tolerance, like real OPP).
        if (loadOn && v * i > El15Protocol.MAX_POWER_W * 1.02f) {
            val disc = emf * emf - 4 * seriesR * El15Protocol.MAX_POWER_W
            i = if (disc > 0f) (emf - sqrt(disc)) / (2 * seriesR)
                else El15Protocol.MAX_POWER_W / max(v, 0.1f)
            v = max(emf - i * seriesR, 0f)
            warn = "OPP"
        } else if (loadOn && iRaw > El15Protocol.MAX_CURRENT_A * 1.001f) {
            warn = "OCP"
        }
        return Triple(v.withNoise(0.0015f), i.withNoise(0.0015f), warn)
    }

    /** Ideal (unclamped) terminal voltage and current for the active mode. */
    private fun solve(): Pair<Float, Float> {
        if (!loadOn) return emf to 0f
        // Physical ceiling: terminal voltage cannot fall below ~0.3 V.
        val iCeil = (emf - 0.3f) / seriesR
        val i = when (mode) {
            El15Protocol.MODE_CC, El15Protocol.MODE_CAP, El15Protocol.MODE_DCR ->
                min(setpoint, iCeil)
            El15Protocol.MODE_CV -> {
                val vSet = min(setpoint, emf)
                max((emf - vSet) / seriesR, 0f)
            }
            El15Protocol.MODE_CR -> {
                val rLoad = max(setpoint, 0.01f)
                emf / (seriesR + rLoad)
            }
            El15Protocol.MODE_CP -> {
                // Solve V*I = P with V = emf - I*R  ->  R*I^2 - emf*I + P = 0
                val p = setpoint
                val disc = emf * emf - 4 * seriesR * p
                if (disc <= 0f) iCeil else min((emf - sqrt(disc)) / (2 * seriesR), iCeil)
            }
            else -> min(setpoint, iCeil)
        }
        val current = max(i, 0f)
        val voltage = max(emf - current * seriesR, 0f)
        return voltage to current
    }

    private fun buildStatus(voltage: Float, current: Float, warn: String): El15Status {
        val s = El15Status()
        s.valid = true
        s.crcPass = true
        s.warning = warn
        s.mode = mode
        s.modeName = El15Protocol.MODE_NAMES[mode] ?: "CC"
        s.voltage = voltage
        s.current = current
        s.power = voltage * current
        s.loadOn = loadOn
        s.lockOn = lockOn
        s.runtime = runtimeSec
        s.temperature = 24.5f + current * 0.9f // warms with load
        s.fanSpeed = when {
            current > 8f -> El15Protocol.FAN_SPEED_MAX
            current > 4f -> 3
            current > 1f -> 1
            else -> 0
        }
        s.ready = loadOn && warn.isEmpty()

        val info = El15Protocol.MODE_SETPOINT_INFO[mode]
        if (info != null) {
            s.setpointUnit = info.unit
            s.setpointDecimals = info.decimals
            s.setpointLabel = info.label
        }
        when (mode) {
            El15Protocol.MODE_CAP -> {
                s.energyWh = energyWh
                s.capacityAh = capacityAh
                s.setpointInPacket = false
            }
            El15Protocol.MODE_DCR -> {
                s.dcrMilliOhm = seriesR * 1000f
                s.dcrI1 = current
                s.dcrI2 = current * 0.5f
                s.current = 0f
                s.power = 0f
                s.setpointInPacket = false
            }
            else -> {
                s.setpoint = setpoint
                s.setpointInPacket = true
            }
        }
        return s
    }

    // Deterministic xorshift noise in ±[frac] of the value.
    private fun Float.withNoise(frac: Float): Float {
        rngState = rngState xor (rngState shl 13)
        rngState = rngState xor (rngState ushr 17)
        rngState = rngState xor (rngState shl 5)
        val unit = (abs(rngState % 1000) / 1000f) * 2f - 1f // -1..1
        return this * (1f + unit * frac)
    }

    companion object {
        private const val POLL_MS = 500L
    }
}
