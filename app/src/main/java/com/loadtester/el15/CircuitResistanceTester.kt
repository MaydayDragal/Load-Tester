package com.loadtester.el15

import android.os.Handler
import android.os.Looper
import kotlin.math.max
import kotlin.math.min

/**
 * Dynamic circuit-resistance test.
 *
 * Drives the EL15 in constant-current (CC) mode to sink a stepped sequence of
 * currents from the circuit under test, sampling terminal voltage and current
 * at each step. The circuit's series resistance (source + wiring + connections)
 * is the slope of the V–I line: V = Voc − R·I, so R = −dV/dI.
 *
 * The current ladder is derived from the circuit's **fuse rating**: the test
 * never exceeds [safetyFactor] × fuse rating (and an absolute hard cap), and the
 * step size scales with the fuse rating — a 2 A fused circuit is probed in tens
 * of milliamps, a 30 A circuit in amps.
 *
 * The engine is a small timer-driven state machine. It is fed live device
 * readings through [onStatus]; the host forwards each status notification while
 * a test is running.
 */
class CircuitResistanceTester(
    private val ble: El15Controller,
    private val callback: Callback,
) {
    interface Callback {
        fun onTestProgress(step: Int, totalSteps: Int, targetCurrent: Float, voltage: Float, current: Float)
        fun onTestComplete(result: ResistanceResult)
        fun onTestError(message: String)
    }

    data class Sample(val current: Float, val voltage: Float)

    data class ResistanceResult(
        val samples: List<Sample>,
        val resistanceOhm: Float,
        val openCircuitVoltage: Float,
        val rSquared: Float,
        val fuseRating: Float,
        val maxTestCurrent: Float,
        val reliable: Boolean,
    )

    // ---- Tunable parameters ----------------------------------------------
    /** Number of current steps in the sweep. */
    var steps: Int = 8
    /** Fraction of the fuse rating the test is allowed to reach (headroom). */
    var safetyFactor: Float = 0.8f
    /** Settling time after changing the setpoint before sampling (ms). */
    var settleMs: Long = 800L
    /** Sampling window per step (ms); readings arrive at the poll rate (~2 Hz). */
    var collectMs: Long = 1500L

    val running: Boolean get() = state != State.IDLE

    private enum class State { IDLE, PRIMING, SETTLING, COLLECTING }

    private val main = Handler(Looper.getMainLooper())
    private var state = State.IDLE

    private val targets = ArrayList<Float>()
    private val results = ArrayList<Sample>()
    private val stepBuffer = ArrayList<Sample>()
    private var stepIndex = 0
    private var fuseRating = 0f
    private var maxCurrent = 0f

    private val pending = ArrayList<Runnable>()

    /** Largest current the test will ever command, regardless of fuse input. */
    private val absoluteMaxCurrent = ABS_MAX_CURRENT

    fun start(fuseRatingAmps: Float) {
        if (running) return
        fuseRating = fuseRatingAmps
        maxCurrent = min(fuseRatingAmps * safetyFactor, absoluteMaxCurrent)
        val n = steps.coerceIn(3, 20)
        targets.clear()
        for (k in 1..n) targets.add(maxCurrent * k / n)
        results.clear()
        stepBuffer.clear()
        stepIndex = 0

        // Prime: CC mode, load off, zero setpoint. Then start the ladder.
        state = State.PRIMING
        ble.setMode(El15Protocol.MODE_CC)
        ble.setSetpoint(0f)
        ble.setLoad(false)
        schedule({ beginStep(0) }, PRIME_MS)
    }

    /** Abort the test and safely turn the load off. */
    fun stop() {
        if (!running) return
        finishSafely()
        state = State.IDLE
    }

    /** Feed a live device reading into the running test. */
    fun onStatus(status: El15Status) {
        if (!running) return
        if (status.warning.isNotEmpty()) {
            abort("Load protection tripped (${status.warning}). Test stopped.")
            return
        }
        if (state == State.COLLECTING) {
            stepBuffer.add(Sample(status.current, status.voltage))
        }
    }

    // ---- State machine ----------------------------------------------------
    private fun beginStep(idx: Int) {
        if (!running) return
        if (idx >= targets.size) {
            complete()
            return
        }
        stepIndex = idx
        val target = targets[idx]
        ble.setSetpoint(target)
        if (idx == 0) ble.setLoad(true) // energise the load once, on the first step

        state = State.SETTLING
        schedule({
            stepBuffer.clear()
            state = State.COLLECTING
            schedule({ endStep() }, collectMs)
        }, settleMs)
    }

    private fun endStep() {
        if (!running) return
        state = State.SETTLING
        val target = targets[stepIndex]
        if (stepBuffer.isNotEmpty()) {
            val i = stepBuffer.map { it.current.toDouble() }.average().toFloat()
            val v = stepBuffer.map { it.voltage.toDouble() }.average().toFloat()
            results.add(Sample(i, v))
            callback.onTestProgress(stepIndex + 1, targets.size, target, v, i)
        }
        beginStep(stepIndex + 1)
    }

    private fun complete() {
        finishSafely()
        state = State.IDLE
        callback.onTestComplete(computeResult())
    }

    private fun abort(message: String) {
        finishSafely()
        state = State.IDLE
        callback.onTestError(message)
    }

    private fun finishSafely() {
        clearPending()
        ble.setLoad(false)
        ble.setSetpoint(0f)
    }

    // ---- Result computation ----------------------------------------------
    private fun computeResult(): ResistanceResult {
        val n = results.size
        if (n < 2) {
            return ResistanceResult(results.toList(), 0f, 0f, 0f, fuseRating, maxCurrent, false)
        }
        var sumI = 0.0; var sumV = 0.0
        for (s in results) { sumI += s.current; sumV += s.voltage }
        val meanI = sumI / n; val meanV = sumV / n

        var sII = 0.0; var sIV = 0.0; var sVV = 0.0
        for (s in results) {
            val di = s.current - meanI
            val dv = s.voltage - meanV
            sII += di * di; sIV += di * dv; sVV += dv * dv
        }

        // Slope of V vs I. V = a + b·I, resistance R = -b.
        val slope = if (sII > 1e-9) sIV / sII else 0.0
        val intercept = meanV - slope * meanI
        val resistance = max(-slope, 0.0).toFloat()
        val rSquared = if (sII > 1e-9 && sVV > 1e-9) ((sIV * sIV) / (sII * sVV)).toFloat() else 0f

        // Consider the fit reliable only with a clear downward trend and spread.
        val currentSpread = (results.maxOf { it.current } - results.minOf { it.current })
        val reliable = n >= 3 && slope < 0 && rSquared >= 0.90f && currentSpread > 0.05f

        return ResistanceResult(
            samples = results.toList(),
            resistanceOhm = resistance,
            openCircuitVoltage = intercept.toFloat(),
            rSquared = rSquared,
            fuseRating = fuseRating,
            maxTestCurrent = maxCurrent,
            reliable = reliable,
        )
    }

    // ---- Timer bookkeeping ------------------------------------------------
    private fun schedule(action: () -> Unit, delay: Long) {
        val r = Runnable { action() }
        pending.add(r)
        main.postDelayed(r, delay)
    }

    private fun clearPending() {
        for (r in pending) main.removeCallbacks(r)
        pending.clear()
    }

    companion object {
        private const val PRIME_MS = 900L
        const val ABS_MAX_CURRENT = 40f
    }
}
