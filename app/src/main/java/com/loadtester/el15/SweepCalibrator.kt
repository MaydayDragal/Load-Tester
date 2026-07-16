package com.loadtester.el15

import android.os.Handler
import android.os.Looper
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.sqrt

/**
 * Auto-calibration for the circuit-resistance sweep.
 *
 * Empirically tunes the three sweep options against the circuit that is
 * actually connected, balancing accuracy, speed, and detail:
 *
 *  1. **Settle probe** — steps the load to ~half the safe test current and
 *     records the voltage trajectory; the measured stabilization time (plus
 *     margin) becomes the recommended settle window.
 *  2. **Noise probe** — holds that current and measures the steady-state
 *     voltage noise; from σ it derives how many readings must be averaged for
 *     the fit error target, giving the sample window.
 *  3. **Convergence sweeps** — runs short real sweeps (5 → 8 → 12 steps) with
 *     the tuned timing and picks the smallest step count where the measured
 *     resistance stops moving (<0.5 %) with a healthy R².
 *
 * Safety mirrors [CircuitResistanceTester]: currents are clamped to
 * min(safety·fuse, 12 A, 150 W/Voc), any protection trip aborts, and the load
 * is always commanded off on every exit path.
 */
class SweepCalibrator(
    private val ble: El15Controller,
    private val callback: Callback,
) {
    interface Callback {
        fun onCalProgress(message: String)
        fun onCalComplete(result: CalResult)
        fun onCalError(message: String)
    }

    data class CalResult(
        val settleMs: Long,
        val sampleMs: Long,
        val steps: Int,
        val stabilizationMs: Long,
        val noiseVolts: Float,
        val openCircuitVoltage: Float,
        val sweepResistances: List<Float>,
        val converged: Boolean,
        val estimatedTestS: Long,
    )

    var safetyFactor: Float = 0.8f
    var pollIntervalMs: Long = 500L

    val running: Boolean get() = state != State.IDLE

    private enum class State { IDLE, PRIME, STEP_TRACE, NOISE, SWEEPS }

    private val main = Handler(Looper.getMainLooper())
    private var state = State.IDLE

    private var fuseRating = 0f
    private var vOc = 0f
    private var iCal = 0f
    private var traceStartMs = 0L
    private val trace = ArrayList<Pair<Long, Float>>()   // (ms since step, V)
    private val noise = ArrayList<Float>()

    private var settleRec = 800L
    private var sampleRec = 1500L

    private val sweepSteps = listOf(5, 8, 12)
    private var sweepIndex = 0
    private val sweepR = ArrayList<Float>()
    private val sweepR2 = ArrayList<Float>()
    private var innerTester: CircuitResistanceTester? = null

    private val pending = ArrayList<Runnable>()

    fun start(fuseRatingAmps: Float) {
        if (running) return
        fuseRating = fuseRatingAmps
        vOc = 0f; iCal = 0f
        trace.clear(); noise.clear()
        sweepIndex = 0; sweepR.clear(); sweepR2.clear()

        state = State.PRIME
        callback.onCalProgress("Calibrating 1/4 — measuring open-circuit voltage…")
        ble.setMode(El15Protocol.MODE_CC)
        ble.setSetpoint(0f)
        ble.setLoad(false)
        schedule({ beginStepTrace() }, maxOf(1200L, 2 * pollIntervalMs + 300))
    }

    fun stop() {
        if (!running) return
        innerTester?.stop()
        innerTester = null
        finishSafely()
        state = State.IDLE
    }

    fun onStatus(status: El15Status) {
        if (!running || !status.valid) return
        if (status.warning.isNotEmpty()) {
            abort("Load protection tripped (${status.warning}). Calibration stopped.")
            return
        }
        when (state) {
            State.PRIME -> vOc = status.voltage
            State.STEP_TRACE ->
                trace.add((System.currentTimeMillis() - traceStartMs) to status.voltage)
            State.NOISE -> noise.add(status.voltage)
            State.SWEEPS -> innerTester?.onStatus(status)
            else -> {}
        }
    }

    // ---- Phase 2: step-response trace --------------------------------------
    private fun beginStepTrace() {
        if (!running) return
        if (vOc < El15Protocol.MIN_VOLTAGE_V) {
            abort("No reading from the load — cannot calibrate."); return
        }
        if (vOc > El15Protocol.MAX_VOLTAGE_V) {
            abort("Source is above the EL15's 60 V rating."); return
        }
        val peak = minOf(fuseRating * safetyFactor, El15Protocol.MAX_CURRENT_A,
            El15Protocol.MAX_POWER_W / vOc)
        iCal = peak * 0.5f
        if (iCal < 0.05f) {
            abort("Safe calibration current is too low — check the fuse rating."); return
        }

        state = State.STEP_TRACE
        callback.onCalProgress("Calibrating 2/4 — measuring settling time at %.2f A…".format(iCal))
        trace.clear()
        traceStartMs = System.currentTimeMillis()
        ble.setSetpoint(iCal)
        ble.setLoad(true)
        schedule({ finishStepTrace() }, maxOf(3000L, 8 * pollIntervalMs))
    }

    private fun finishStepTrace() {
        if (!running) return
        val stab = CalMath.stabilizationTimeMs(trace, vOc)
        settleRec = if (stab != null) {
            // 20% margin, rounded up to 100 ms; never below one poll interval.
            (((stab * 1.2).toLong() + 99) / 100 * 100).coerceIn(pollIntervalMs, 5000L)
        } else {
            maxOf(2 * pollIntervalMs, 800L) // trace too sparse — safe default
        }
        beginNoise()
    }

    // ---- Phase 3: noise probe -----------------------------------------------
    private fun beginNoise() {
        state = State.NOISE
        callback.onCalProgress("Calibrating 3/4 — measuring noise floor…")
        noise.clear()
        schedule({ finishNoise() }, maxOf(3000L, 8 * pollIntervalMs))
    }

    private fun finishNoise() {
        if (!running) return
        val vLoaded = if (noise.isNotEmpty()) noise.average().toFloat() else vOc
        val sigma = CalMath.sigma(noise)
        val deltaV = abs(vOc - vLoaded)
        sampleRec = CalMath.sampleWindowMs(sigma, deltaV, pollIntervalMs).coerceAtMost(8000L)
        // Probe done — release the load before the mini-sweeps re-prime.
        ble.setLoad(false)
        ble.setSetpoint(0f)
        beginSweep()
    }

    // ---- Phase 4: convergence sweeps ----------------------------------------
    private fun beginSweep() {
        if (!running) return
        if (sweepIndex >= sweepSteps.size) { complete(); return }
        state = State.SWEEPS
        val n = sweepSteps[sweepIndex]
        callback.onCalProgress(
            "Calibrating 4/4 — sweep ${sweepIndex + 1}/${sweepSteps.size} ($n steps)…")
        val t = CircuitResistanceTester(ble, object : CircuitResistanceTester.Callback {
            override fun onTestProgress(step: Int, totalSteps: Int, targetCurrent: Float, voltage: Float, current: Float) {
                callback.onCalProgress(
                    "Calibrating 4/4 — sweep ${sweepIndex + 1}/${sweepSteps.size} · step $step/$totalSteps")
            }
            override fun onTestComplete(result: CircuitResistanceTester.ResistanceResult) {
                sweepR.add(result.resistanceOhm)
                sweepR2.add(result.rSquared)
                innerTester = null
                sweepIndex++
                // Early exit: R already stable after two sweeps.
                if (sweepR.size >= 2 && CalMath.converged(sweepR)) {
                    complete()
                } else {
                    beginSweep()
                }
            }
            override fun onTestError(message: String) {
                innerTester = null
                abort("Calibration sweep failed: $message")
            }
        }).apply {
            steps = n
            settleMs = settleRec
            collectMs = sampleRec
            safetyFactor = this@SweepCalibrator.safetyFactor
            pollIntervalMs = this@SweepCalibrator.pollIntervalMs
        }
        innerTester = t
        t.start(fuseRating)
    }

    private fun complete() {
        finishSafely()
        state = State.IDLE
        val (steps, converged) = CalMath.chooseSteps(sweepR, sweepR2, sweepSteps)
        val perStep = maxOf(settleRec, pollIntervalMs + 100) +
            maxOf(sampleRec, 2 * pollIntervalMs + 200)
        callback.onCalComplete(
            CalResult(
                settleMs = settleRec,
                sampleMs = sampleRec,
                steps = steps,
                stabilizationMs = settleRec,
                noiseVolts = CalMath.sigma(noise),
                openCircuitVoltage = vOc,
                sweepResistances = sweepR.toList(),
                converged = converged,
                estimatedTestS = steps.toLong() * perStep / 1000L,
            )
        )
    }

    private fun abort(message: String) {
        innerTester?.stop()
        innerTester = null
        finishSafely()
        state = State.IDLE
        callback.onCalError(message)
    }

    private fun finishSafely() {
        for (r in pending) main.removeCallbacks(r)
        pending.clear()
        ble.setLoad(false)
        ble.setSetpoint(0f)
    }

    private fun schedule(action: () -> Unit, delay: Long) {
        val r = Runnable { action() }
        pending.add(r)
        main.postDelayed(r, delay)
    }

    /** Pure math, unit-testable on the JVM. */
    object CalMath {

        /**
         * Time until the step response settles: the earliest trace time after
         * which every reading stays inside a noise band around the final value.
         *
         * The band is derived from successive *differences* in the tail rather
         * than the tail's standard deviation — a still-drifting signal then
         * keeps a tight band (drift is not noise) and correctly reports a late
         * or never stabilization instead of a falsely early one.
         *
         * Returns null when the trace is too sparse to judge (<4 samples).
         */
        fun stabilizationTimeMs(trace: List<Pair<Long, Float>>, vStart: Float): Long? {
            if (trace.size < 4) return null
            val tail4 = trace.takeLast(4).map { it.second }
            val diffs = (1 until tail4.size).map { tail4[it] - tail4[it - 1] }
            val noiseSigma = sigma(diffs)
            val tailMean = trace.takeLast(3).map { it.second }.average().toFloat()
            val deltaV = abs(vStart - tailMean)
            val band = maxOf(3f * noiseSigma, 0.002f * deltaV, 0.005f)
            for (k in trace.indices) {
                if ((k until trace.size).all { abs(trace[it].second - tailMean) <= band }) {
                    return trace[k].first
                }
            }
            return trace.last().first
        }

        /** Sample standard deviation. */
        fun sigma(values: List<Float>): Float {
            if (values.size < 2) return 0f
            val mean = values.average()
            val varSum = values.sumOf { (it - mean) * (it - mean) }
            return sqrt(varSum / (values.size - 1)).toFloat()
        }

        /**
         * Sample window needed to average voltage noise below the fit-error
         * target: SE = σ/√n ≤ max(1 mV, 0.1 % of the loaded voltage drop).
         */
        fun sampleWindowMs(sigmaV: Float, deltaV: Float, pollMs: Long): Long {
            val target = maxOf(0.001f, 0.001f * deltaV)
            val n = if (sigmaV <= 0f) 2
            else ceil((sigmaV / target) * (sigmaV / target).toDouble()).toInt().coerceIn(2, 20)
            return n * pollMs + pollMs / 2
        }

        /** True when the last two measurements agree within 0.5 %. */
        fun converged(rs: List<Float>): Boolean {
            if (rs.size < 2) return false
            val a = rs[rs.size - 2]; val b = rs.last()
            if (a <= 0f) return false
            return abs(b - a) / a < 0.005f
        }

        /**
         * Pick the recommended step count: the count one tier above where R
         * converged (stability proven, extra detail nearly free); if it never
         * converged, go one tier past the largest tried for headroom.
         */
        fun chooseSteps(rs: List<Float>, r2s: List<Float>, tried: List<Int>): Pair<Int, Boolean> {
            for (i in 1 until rs.size) {
                if (rs[i - 1] > 0f && abs(rs[i] - rs[i - 1]) / rs[i - 1] < 0.005f &&
                    (r2s.getOrNull(i) ?: 0f) >= 0.99f
                ) {
                    return tried.getOrElse(minOf(i + 1, tried.size - 1)) { tried.last() } to true
                }
            }
            return (tried.lastOrNull() ?: 12) + 4 to false
        }
    }
}
