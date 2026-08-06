package com.loadtester.el15

import android.os.Handler
import android.os.Looper
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

/**
 * Running ordinary-least-squares fit of V = Voc - R*I.
 *
 * Kept as its own pure class so the fit maths is unit-testable without any of
 * the timer machinery, and so the live estimate costs nothing: the whole sample
 * history lives in six doubles, the regression is available at any instant, and
 * nothing grows during a sweep however many packets arrive.
 */
class LeastSquares {
    var n = 0L
        private set
    private var sumI = 0.0
    private var sumV = 0.0
    private var sumII = 0.0
    private var sumIV = 0.0
    private var sumVV = 0.0

    var minI = 0f; private set
    var maxI = 0f; private set
    var minV = 0f; private set
    var maxV = 0f; private set

    /** Fitted line plus the slope's 1-sigma uncertainty. */
    data class Fit(
        /** Resistance in ohms: the negated slope, floored at zero. */
        val resistanceOhm: Float,
        /** Voc — the fitted intercept at zero current. */
        val openCircuitVoltage: Float,
        val rSquared: Float,
        /** 1-sigma standard error of the resistance (ohms); the +/- tolerance. */
        val resistanceStdErr: Float,
        val slopeNegative: Boolean,
    )

    fun add(i: Float, v: Float) {
        val di = i.toDouble(); val dv = v.toDouble()
        n++
        sumI += di; sumV += dv
        sumII += di * di; sumIV += di * dv; sumVV += dv * dv
        if (n == 1L) {
            minI = i; maxI = i; minV = v; maxV = v
        } else {
            minI = min(minI, i); maxI = max(maxI, i)
            minV = min(minV, v); maxV = max(maxV, v)
        }
    }

    fun reset() {
        n = 0; sumI = 0.0; sumV = 0.0; sumII = 0.0; sumIV = 0.0; sumVV = 0.0
        minI = 0f; maxI = 0f; minV = 0f; maxV = 0f
    }

    /** Realized current span across the samples seen so far. */
    val currentSpan: Float get() = maxI - minI

    /**
     * Fit the line, or null if the samples so far cannot define one — fewer than
     * [MIN_SAMPLES] points, or a current span too narrow to give the slope any
     * leverage. Callers must not show a resistance before this returns non-null.
     */
    fun fit(): Fit? {
        if (n < MIN_SAMPLES) return null
        val nn = n.toDouble()
        val sII = sumII - sumI * sumI / nn
        val sIV = sumIV - sumI * sumV / nn
        val sVV = sumVV - sumV * sumV / nn
        if (sII < 1e-9 || currentSpan < MIN_SPAN_A) return null
        val slope = sIV / sII
        val intercept = sumV / nn - slope * (sumI / nn)
        val rSquared = if (sVV > 1e-9) ((sIV * sIV) / (sII * sVV)).toFloat() else 0f
        // SSE = S_VV - slope*S_IV; Var(slope) = (SSE/(n-2)) / S_II.
        val sse = (sVV - slope * sIV).coerceAtLeast(0.0)
        val stdErr = if (n > 2) sqrt(sse / ((nn - 2) * sII)) else 0.0
        return Fit(
            max(-slope, 0.0).toFloat(), intercept.toFloat(), rSquared,
            stdErr.toFloat(), slope < 0,
        )
    }

    companion object {
        const val MIN_SAMPLES = 8
        const val MIN_SPAN_A = 0.05f
    }
}

/**
 * Fuse-aware circuit-resistance test — continuous triangular current sweep.
 *
 * Kotlin port of the firmware's `resistance_test.h`.
 *
 * Drives the load in CC mode along a SMOOTH current ramp: up from a starting
 * current to a maximum over half the sweep, then back down over the other half.
 * Every status packet that arrives on the way is a data point, and the series
 * resistance is the slope of the V-I line through them: V = Voc - R*I, so
 * R = -dV/dI.
 *
 * Why a ramp rather than the discrete ladder this replaced: the old design had
 * to sit at each current level for a settle window plus a collect window
 * (~2.3 s per level), which bought only ~10 usable readings per level and made
 * the sweep length a function of the step count rather than something the user
 * could choose. A continuous ramp uses EVERY packet — a 30 s sweep at a 20 Hz
 * poll yields ~300 points instead of ~80 — and the sweep duration becomes a
 * plain, editable number. It also produces a live curve worth watching.
 *
 * Accuracy techniques (see the firmware's RTEST_ACCURACY.md):
 *  1. BIDIRECTIONAL — the ramp is a symmetric triangle, so each current is
 *     visited once on the way up and once on the way down, equally spaced about
 *     the sweep midpoint. First-order time drift (wire self-heating, battery
 *     sag) therefore cancels in the fit instead of biasing the slope.
 *  2. UNCERTAINTY — reports the 1-sigma standard error of the fitted slope
 *     (R +/- sigma) and gates "reliable" on that tolerance rather than R^2,
 *     which is meaningful at any resistance scale.
 *  3. SAMPLE DENSITY — every packet counts, so a faster poll rate directly
 *     tightens the fit. The regression runs incrementally, so the live R
 *     estimate costs nothing and no sample buffer grows.
 *
 * V and I inside one status frame are simultaneous, which is what makes a moving
 * setpoint safe to fit: the load's regulation lag shifts WHICH current a packet
 * reports, not the V that goes with it, so the V-I relationship is undistorted.
 *
 * Timer-driven state machine pumped by an internal ticker and fed live readings
 * via [onStatus]. Every commanded current is clamped to the EL15's
 * 150 W / 60 V / 12 A ratings, and every exit path leaves the load OFF.
 *
 * Main-thread confined, like the transports that feed it.
 */
class ResistanceTest(
    private val ble: El15Controller,
    private val callback: Callback,
) {
    data class Sample(
        val current: Float,
        val voltage: Float,
        val temperature: Float = 0f,
        val fanSpeed: Int = 0,
    ) {
        val power: Float get() = current * voltage
    }

    data class Result(
        /** One binned point per current band. */
        val samples: List<Sample>,
        val resistanceOhm: Float,
        val openCircuitVoltage: Float,
        val rSquared: Float,
        /** 1-sigma uncertainty on [resistanceOhm], in ohms. */
        val resistanceStdErr: Float,
        val fuseRating: Float,
        val maxTestCurrent: Float,
        /** Commanded ramp floor. */
        val startCurrent: Float,
        /** Commanded ramp duration. */
        val sweepSeconds: Long,
        /** Status packets actually used in the fit. */
        val rawSamples: Int,
        val reliable: Boolean,
        // Run statistics, taken over EVERY raw sample rather than the binned
        // curve, so they report what the load actually saw.
        val minCurrent: Float,
        val maxCurrentSeen: Float,
        /** Full voltage swing across the sweep. */
        val sagV: Float,
        val peakPowerW: Float,
        val tempMin: Float,
        val tempMax: Float,
        val maxFan: Int,
        /** Times the load had to be re-asserted mid-sweep. */
        val loadDropouts: Int,
        /**
         * What the slope actually measured, before the lead tare was taken off.
         * This is also what a tare run itself records.
         */
        val rawResistanceOhm: Float,
        /** Subtracted (0 in 4-wire, where there is nothing to subtract). */
        val tareOhm: Float,
        val fourWire: Boolean,
    )

    interface Callback {
        /**
         * [rValid] is false until the sweep has covered enough current span for
         * the running fit to mean anything — the UI must not show a number
         * before then.
         */
        fun onTestProgress(
            elapsedS: Float, totalS: Float, target: Float,
            voltage: Float, current: Float, resistance: Float, rValid: Boolean,
        )

        fun onTestComplete(result: Result)
        fun onTestError(message: String)
    }

    // ---- Probe wiring --------------------------------------------------------
    /**
     * In 4-wire (Kelvin) sensing the voltage is measured through a second pair of
     * leads that carry no current, so lead and contact resistance never enters
     * the reading and there is nothing to subtract. In 2-wire it is measured in
     * series with the circuit under test, so [tareOhm] — captured by running a
     * sweep with the probes shorted together — is taken off the result. At the
     * milliohm scale that correction is often larger than what is being
     * measured, which is why this matters.
     */
    var fourWire: Boolean = false
    var tareOhm: Float = 0f

    // ---- Sweep shape ---------------------------------------------------------
    /**
     * Where the ramp begins (usually 0) and where it turns around; both are
     * clamped to what the fuse rating and the EL15's ratings allow, so a user
     * asking for too much gets a safe sweep rather than a refusal.
     * [maxCurrent] <= 0 means "use the whole safe headroom the fuse allows".
     */
    var startCurrent: Float = 0f
    var maxCurrent: Float = 0f
    var sweepSeconds: Long = 30

    var safetyFactor: Float = 0.8f
    var pollIntervalMs: Long = 50

    /**
     * How often the ramp re-commands the setpoint. 0 = use [RAMP_STEP_MS].
     * It must stay SLOWER than the poll interval: every control write defers the
     * next poll, so updating faster than we poll would starve telemetry and the
     * sweep would collect nothing.
     */
    var setpointMs: Long = 0

    /**
     * Ignore samples for this long after the load is switched on, to skip the
     * regulator's start-up transient. 0 = use everything.
     */
    var settleMs: Long = 0

    val running: Boolean get() = state != State.IDLE

    private enum class State { IDLE, PRIMING, ARMING, SWEEPING }

    private val main = Handler(Looper.getMainLooper())
    private var state = State.IDLE

    private val lsq = LeastSquares()
    private val bins = Array(NBINS) { Bin() }

    private var tempMin = 0f
    private var tempMax = 0f
    private var haveTemp = false
    private var fanMax = 0
    private var peakW = 0f

    private var fuseRating = 0f
    private var maxCurrentEff = 0f
    private var startCurrentEff = 0f
    private var currentTarget = 0f
    private var vOcMeasured = 0f
    private var vRecent = 0f

    private var tStart = 0L
    private var lastSetMs = 0L
    private var lastOnPushMs = 0L
    private var primeStartMs = 0L
    private var lastClearPushMs = 0L
    private var primeDoneAtMs = 0L
    private var armStartMs = 0L
    private var sweepMsEff = 30_000L
    private var loadDropouts = 0
    private var logging = false

    private class Bin {
        var sumI = 0.0; var sumV = 0.0; var sumT = 0.0; var count = 0; var fanMax = 0
        fun reset() { sumI = 0.0; sumV = 0.0; sumT = 0.0; count = 0; fanMax = 0 }
    }

    // ---- Lifecycle -----------------------------------------------------------
    fun start(fuseRatingAmps: Float) {
        if (running) return
        fuseRating = fuseRatingAmps
        resetAccumulators()
        vOcMeasured = 0f
        vRecent = 0f
        loadDropouts = 0
        state = State.PRIMING
        primeStartMs = now()
        lastClearPushMs = 0
        ble.setMode(El15Protocol.MODE_CC)
        ble.setSetpoint(0f)
        ble.setLoad(false)
        primeDoneAtMs = now() + primeMs()
        startTicker()
    }

    fun stop() {
        if (!running) return
        finishSafely()
        state = State.IDLE
    }

    // ---- Live readings -------------------------------------------------------
    /**
     * Feed a live reading into the running test.
     *
     * Note the host must pass the RAW packet here, not a probe-compensated one:
     * the fit already removes the lead tare from the SLOPE (see [correct]), and a
     * pre-corrected voltage would take the same resistance off a second time —
     * the sweep would report a DUT one tare too low, and a low-milliohm result
     * could land at zero.
     */
    fun onStatus(status: El15Status) {
        if (!running || !status.valid) return
        vRecent = status.voltage

        if (state == State.PRIMING) {
            // A protection latched by a PREVIOUS run — a link drop or an app kill
            // that stranded the load, say — must not veto this one forever.
            // Priming already commands mode CC / setpoint 0 / LOAD OFF, which is
            // exactly the state that lets the device clear itself, so keep
            // pushing that and give it a grace period before giving up. Only a
            // trip that survives the grace period is a real reason not to start.
            if (status.warning.isNotEmpty()) {
                if (now() - primeStartMs > FAULT_CLEAR_MS) {
                    abort("Load protection will not clear. Check the setup, then retry.")
                    return
                }
                if (now() - lastClearPushMs > 400) {
                    lastClearPushMs = now()
                    ble.setLoad(false)
                    ble.setSetpoint(0f)
                }
                // Don't let the prime timer expire while the device is still
                // faulted, and don't trust this packet as an open-circuit reading.
                primeDoneAtMs = now() + primeMs()
                return
            }
            vOcMeasured = status.voltage
            return
        }

        // Mid-sweep a protection trip is real and immediate: current is flowing.
        if (status.warning.isNotEmpty()) {
            abort("Load protection tripped (${status.warning}). Test stopped.")
            return
        }

        // Waiting for the load to pick up. Re-assert on the same cadence the
        // sweep uses, and only start the ramp once current is genuinely flowing
        // — the loadOn bit alone is not enough, since the device can report the
        // load on while still regulating in from zero.
        if (state == State.ARMING) {
            if (status.loadOn && status.current > ARM_MIN_CURRENT) { beginSweep(); return }
            if (now() - lastOnPushMs > LOAD_REASSERT_MS) {
                lastOnPushMs = now()
                loadDropouts++
                ble.setSetpoint(currentTarget)
                ble.setLoad(true)
            }
            callback.onTestProgress(
                0f, sweepMsEff / 1000f, currentTarget,
                status.voltage, status.current, 0f, false,
            )
            return
        }

        if (state != State.SWEEPING) return

        // Offer every packet to the datapoint log. Logged BEFORE the load-on and
        // settle gates below on purpose: dropout and transient samples are
        // excluded from the FIT, but the CSV should show what actually happened,
        // not only what was fitted. The commanded target rides along so
        // regulation lag is visible in the file (the fit itself never uses it).
        if (logging) {
            SampleLog.rtest.add(
                now() - tStart, status.voltage, status.current,
                status.temperature, currentTarget, status.fanSpeed.toFloat(),
            )
        }

        // The load must actually be sinking for a sample to be part of the sweep.
        // A packet reporting the load off is either the gap before LOAD_ON lands
        // or a dropout, and its (0 A, Voc) point would drag the fit toward the
        // intercept while telling us nothing about the slope. Re-assert LOAD_ON
        // rather than silently sweeping a dead load, but no faster than
        // LOAD_REASSERT_MS so this cannot become a command flood of its own.
        if (!status.loadOn) {
            if (now() - lastOnPushMs > LOAD_REASSERT_MS) {
                lastOnPushMs = now()
                loadDropouts++
                ble.setSetpoint(currentTarget)
                ble.setLoad(true)
            }
            callback.onTestProgress(
                elapsedS(), sweepMsEff / 1000f, currentTarget,
                status.voltage, status.current, 0f, false,
            )
            return
        }

        // Optional start-up transient skip. The load has to establish regulation
        // from off, and those first readings are of that transient rather than of
        // the ramp. Discarded symmetrically in effect: they are all at the ramp's
        // baseline current, so dropping them costs no span.
        if (settleMs > 0 && now() - tStart < settleMs) {
            callback.onTestProgress(
                elapsedS(), sweepMsEff / 1000f, currentTarget,
                status.voltage, status.current, 0f, false,
            )
            return
        }

        lsq.add(status.current, status.voltage)
        // Seed both bounds from the first sample rather than from zero — a
        // below-zero ambient would otherwise never move tempMax off 0.
        if (!haveTemp) {
            tempMin = status.temperature; tempMax = status.temperature; haveTemp = true
        } else {
            tempMin = min(tempMin, status.temperature)
            tempMax = max(tempMax, status.temperature)
        }
        fanMax = max(fanMax, status.fanSpeed)
        peakW = max(peakW, status.voltage * status.current)

        binSample(status)

        val f = lsq.fit()
        callback.onTestProgress(
            elapsedS(), sweepMsEff / 1000f, currentTarget,
            status.voltage, status.current,
            if (f != null) correct(f.resistanceOhm) else 0f, f != null,
        )
    }

    // ---- Ticker --------------------------------------------------------------
    private val ticker = object : Runnable {
        override fun run() {
            if (!running) return
            tick()
            if (running) main.postDelayed(this, TICK_MS)
        }
    }

    private fun startTicker() {
        main.removeCallbacks(ticker)
        main.postDelayed(ticker, TICK_MS)
    }

    private fun tick() {
        if (state == State.SWEEPING) {
            val el = now() - tStart
            if (el >= sweepMsEff) { complete(); return }
            // Re-command the setpoint on a fixed cadence, slower than the poll.
            val step = setpointStepMs()
            if (lastSetMs == 0L || now() - lastSetMs >= step) {
                val since = if (lastSetMs != 0L) now() - lastSetMs else step
                lastSetMs = now()
                var want = clampToRatings(commandable(targetAt(el)))
                // Slew-limit against what we last COMMANDED, so a dropped write
                // becomes a short catch-up instead of a step change at the load.
                val limit = slewLimit(since)
                if (currentTarget > 0f && limit > 0f) {
                    want = currentTarget + (want - currentTarget).coerceIn(-limit, limit)
                }
                currentTarget = want
                ble.setSetpoint(currentTarget)
            }
            return
        }
        // The load never picked up. Say so plainly — a sweep that runs with no
        // current produces no fit anyway, and "not enough usable samples" would
        // send the user looking at the sample rate instead of at their
        // connections.
        if (state == State.ARMING && now() - armStartMs > ARM_MAX_MS) {
            abort(
                "The load never started sinking current. Check the connections " +
                    "and that the source is above the minimum voltage, then retry."
            )
            return
        }
        if (state == State.PRIMING && now() >= primeDoneAtMs) finishPriming()
    }

    // ---- Ramp maths ----------------------------------------------------------
    private fun primeMs(): Long = max(PRIME_MS, 2 * pollIntervalMs + 300)

    /**
     * Lowest current the load will actually accept as a CC operating point. Below
     * this the EL15 reports the load off, so the ramp floors its commanded value
     * here even when the user asked to start at 0 A. The result reports the
     * current range it MEASURED, so the floor is visible rather than hidden.
     */
    private fun commandable(target: Float): Float = max(target, MIN_TEST_CURRENT)

    private fun setpointStepMs(): Long = if (setpointMs > 0) setpointMs else RAMP_STEP_MS

    private fun elapsedS(): Float =
        if (state == State.SWEEPING) (now() - tStart) / 1000f else 0f

    /**
     * Symmetric eased triangle: start -> max over the first half, back to start
     * over the second. Returns the commanded current at [elMs] into the sweep.
     *
     * The easing is applied to the TRIANGLE PARAMETER, so each current is still
     * visited once going up and once coming down, at times symmetric about the
     * sweep midpoint. That symmetry is what cancels first-order time drift in
     * the fit, and it survives unchanged — the samples just sit at slightly
     * different instants than they did.
     */
    private fun targetAt(elMs: Long): Float {
        val half = sweepMsEff / 2.0f
        if (half <= 0f) return startCurrentEff
        val frac = if (elMs < half) elMs / half else 2.0f - elMs / half
        return startCurrentEff + (maxCurrentEff - startCurrentEff) * easeTriangle(frac)
    }

    /**
     * Largest setpoint change a single command may make, given how long it has
     * actually been since the last one.
     *
     * Without this, a command the device DROPS (it silently discards a
     * no-response write landing too close behind another) leaves the load
     * holding a stale setpoint, and the next command that does land asks for the
     * whole accumulated jump at once. That is a current spike, and it is the one
     * the ramp shape cannot smooth away.
     *
     * Capping the per-command delta turns that jump into a short catch-up over
     * the following commands. It costs a little regulation lag and NOTHING in
     * accuracy: V and I inside one status frame are simultaneous, so lag changes
     * which current a packet reports, never the voltage paired with it. The fit
     * never sees the commanded value at all.
     */
    private fun slewLimit(sinceMs: Long): Float {
        val span = maxCurrentEff - startCurrentEff
        val half = sweepMsEff / 2.0f
        if (span <= 0f || half <= 0f) return span
        val ratePerMs = (span / half) / (1f - EASE_FRAC)
        return ratePerMs * sinceMs * SLEW_CATCHUP
    }

    private fun resetAccumulators() {
        lsq.reset()
        for (b in bins) b.reset()
        tempMin = 0f; tempMax = 0f; haveTemp = false
        fanMax = 0; peakW = 0f
        lastSetMs = 0; currentTarget = 0f
    }

    /**
     * Bin by measured current for the result curve and the CSV. Because the ramp
     * visits each bin once going up and once coming down, the two visits average
     * together in the bin, which is where the drift cancellation actually lands.
     */
    private fun binSample(s: El15Status) {
        val span = maxCurrentEff - startCurrentEff
        if (span <= 1e-6f) return
        val k = (((s.current - startCurrentEff) / span) * NBINS).toInt().coerceIn(0, NBINS - 1)
        val b = bins[k]
        b.sumI += s.current; b.sumV += s.voltage; b.sumT += s.temperature
        b.count++
        b.fanMax = max(b.fanMax, s.fanSpeed)
    }

    /**
     * 4-wire already excludes the leads; 2-wire subtracts the measured tare and
     * clamps at zero (a DUT below the tare means the tare is stale, not that
     * resistance went negative).
     */
    private fun correct(raw: Float): Float =
        if (fourWire) raw else max(raw - tareOhm, 0f)

    private fun clampToRatings(target: Float): Float {
        val v = if (vRecent > El15Protocol.MIN_VOLTAGE_V) vRecent else El15Protocol.MAX_VOLTAGE_V
        val powerCap = El15Protocol.MAX_POWER_W / v
        return minOf(target, El15Protocol.MAX_CURRENT_A, powerCap, ABS_MAX_CURRENT)
    }

    // ---- State transitions ---------------------------------------------------
    private fun finishPriming() {
        val voc = vOcMeasured
        if (voc < El15Protocol.MIN_VOLTAGE_V) {
            abort("No reading, or voltage below the minimum. Test aborted."); return
        }
        if (voc > El15Protocol.MAX_VOLTAGE_V) {
            abort("Source above the EL15's 60 V rating. Test aborted."); return
        }

        val cap = safeMaxCurrent(fuseRating, voc, safetyFactor)
        maxCurrentEff = if (maxCurrent > 0f) min(maxCurrent, cap) else cap
        if (maxCurrentEff < MIN_TEST_CURRENT) {
            abort("Safe test current too low - check the fuse rating."); return
        }
        // The ramp's baseline is the lowest current the load will actually hold,
        // NOT zero. Interpolating from 0 and clipping on the way out left a flat,
        // commanded-but-unreachable segment at each end of the triangle — a
        // discontinuity in an otherwise smooth ramp, and dead time that collected
        // samples at a single current. Starting the interpolation AT the floor
        // makes the whole sweep linear and every sample useful.
        startCurrentEff = startCurrent.coerceIn(
            MIN_TEST_CURRENT,
            max(maxCurrentEff - MIN_TEST_CURRENT, MIN_TEST_CURRENT),
        )
        // The fit refuses a sweep whose realized current span is under 0.05 A —
        // catch that HERE, before up to 15 minutes of real current are drawn on a
        // sweep that cannot produce a result. 2x the gate leaves room for the
        // realized span coming in under the commanded one.
        if (maxCurrentEff - startCurrentEff < 2 * MIN_TEST_CURRENT) {
            abort(
                "Sweep current span too small for a fit - raise the peak current " +
                    "or lower the start current."
            )
            return
        }

        sweepMsEff = sweepSeconds.coerceIn(MIN_SWEEP_S, MAX_SWEEP_S) * 1000L

        // Command a real current BEFORE switching the load on. Verified on
        // hardware 2026-08-01: the EL15 accepts LOAD_ON at a 0.000 A CC setpoint
        // and then simply reports the load off — the whole sweep ran with I=0 and
        // the fit had nothing to work with.
        currentTarget = clampToRatings(commandable(startCurrentEff))
        ble.setSetpoint(currentTarget)
        ble.setLoad(true)

        // ARM, don't sweep: the ramp clock must not start until the load is
        // actually sinking current.
        //
        // These two writes go out back to back, and the device DROPS a
        // no-response write that lands too close behind another, so the LOAD_ON
        // is the one most likely to be lost. Pacing makes that rare, not
        // impossible — the load can also just be slow to pick up.
        //
        // Starting the clock here anyway spends the opening of the ramp
        // commanding currents nothing draws, and since the ramp starts at the
        // BOTTOM that costs exactly the low-current end the fit most needs span
        // at. Seen on a real sweep (RTEST_003): LOAD_ON lost, re-asserted twice,
        // conduction at 1.07 s, and a run that commanded 0.05 A reported a
        // measured minimum of 0.169 A.
        state = State.ARMING
        armStartMs = now()
        // Arm the re-assert timer so the FIRST check happens one interval in,
        // not one interval after some later event.
        lastOnPushMs = now()
    }

    /** The load is conducting: start the ramp for real. */
    private fun beginSweep() {
        // Open the datapoint log HERE, so elapsed_s in the report is measured
        // from the first current the sweep actually drew.
        logging = SampleLog.rtest.start()
        state = State.SWEEPING
        tStart = now()
        lastSetMs = now()
        lastOnPushMs = now()
    }

    private fun complete() {
        val f = lsq.fit()
        finishSafely()
        state = State.IDLE
        if (f == null) {
            callback.onTestError(
                "Not enough usable samples - raise the sample rate or lengthen the sweep."
            )
            return
        }

        val curve = ArrayList<Sample>(NBINS)
        for (b in bins) {
            if (b.count == 0) continue
            curve.add(
                Sample(
                    (b.sumI / b.count).toFloat(), (b.sumV / b.count).toFloat(),
                    (b.sumT / b.count).toFloat(), b.fanMax,
                )
            )
        }

        // Reliable when the uncertainty is small in ABSOLUTE (<= 5 mohm) OR
        // RELATIVE (<= 5 %) terms — meaningful at any resistance scale, unlike
        // R^2 which false-alarms on clean low-milliohm reads and rubber-stamps
        // noisy big ones. Judge it against what was MEASURED, not the
        // tare-corrected figure: subtracting a constant cannot make the fit
        // better, and a near-tare result would otherwise look wildly unreliable.
        val relTol =
            if (f.resistanceOhm > 1e-4f) f.resistanceStdErr / f.resistanceOhm else 1e9f
        val reliable = lsq.n >= 20 && lsq.currentSpan > LeastSquares.MIN_SPAN_A &&
            (f.resistanceStdErr <= 0.005f || relTol <= 0.05f)

        callback.onTestComplete(
            Result(
                samples = curve,
                resistanceOhm = correct(f.resistanceOhm),
                openCircuitVoltage = f.openCircuitVoltage,
                rSquared = f.rSquared,
                resistanceStdErr = f.resistanceStdErr,
                fuseRating = fuseRating,
                maxTestCurrent = maxCurrentEff,
                startCurrent = startCurrentEff,
                sweepSeconds = sweepMsEff / 1000,
                rawSamples = lsq.n.toInt(),
                reliable = reliable,
                minCurrent = lsq.minI,
                maxCurrentSeen = lsq.maxI,
                sagV = lsq.maxV - lsq.minV,
                peakPowerW = peakW,
                tempMin = if (haveTemp) tempMin else 0f,
                tempMax = if (haveTemp) tempMax else 0f,
                maxFan = fanMax,
                loadDropouts = loadDropouts,
                rawResistanceOhm = f.resistanceOhm,
                tareOhm = if (fourWire) 0f else tareOhm,
                fourWire = fourWire,
            )
        )
    }

    private fun abort(message: String) {
        finishSafely()
        state = State.IDLE
        callback.onTestError(message)
    }

    private fun finishSafely() {
        main.removeCallbacks(ticker)
        ble.setLoad(false)
        ble.setSetpoint(0f)
    }

    private fun now(): Long = android.os.SystemClock.elapsedRealtime()

    companion object {
        const val MIN_SWEEP_S = 5L
        const val MAX_SWEEP_S = 900L

        private const val PRIME_MS = 900L
        private const val MIN_TEST_CURRENT = 0.05f

        /** Absolute backstop; the EL15's 12 A rating is the real ceiling. */
        const val ABS_MAX_CURRENT = 40f

        /**
         * Current bands for the reported curve. 32 is plenty to draw a V-I line
         * and to show curvature, and keeps the CSV readable.
         */
        private const val NBINS = 32

        /** 10 Hz setpoint updates. */
        private const val RAMP_STEP_MS = 100L
        private const val TICK_MS = 25L

        /**
         * Fraction of each half-ramp spent easing the rate in and out.
         *
         * The raw shape is a symmetric triangle, which has a step change in
         * slope at three places: switch-on, the turnaround at the peak, and the
         * end. At each one the commanded current changes direction instantly,
         * and the load's CC regulator answers that with an overshoot — a visible
         * current spike.
         *
         * [easeTriangle] maps the triangle's position through a TRAPEZOIDAL
         * VELOCITY profile instead: the ramp rate rises from zero over the first
         * EASE_FRAC of each half, holds constant, then falls back to zero. Slope
         * is continuous everywhere, including across the peak.
         *
         * The cost is small and bounded: holding the same span in the same time
         * with two eased ends needs a plateau rate of 1/(1 - EASE_FRAC), about
         * 9 % above the linear rate here. That matters because a LINEAR ramp
         * already minimises the peak rate of change for a given span and
         * duration — any smoothing has to buy its soft corners somewhere, and
         * 9 % is the whole bill. (A raised cosine, the obvious alternative,
         * costs 57 %.)
         */
        const val EASE_FRAC = 0.08f

        /**
         * How much faster than the nominal ramp rate a single command may move,
         * so a gap left by a dropped write closes over a few commands rather
         * than in one.
         */
        private const val SLEW_CATCHUP = 1.5f

        /** The eased triangle parameter. Pure, so the profile is unit-testable. */
        fun easeTriangle(tRaw: Float): Float {
            val t = tRaw.coerceIn(0f, 1f)
            val e = EASE_FRAC
            val v = 1f / (1f - e)   // plateau rate that still covers 0..1
            if (t < e) return v * t * t / (2f * e)
            if (t <= 1f - e) return v * (t - e / 2f)
            val d = 1f - t
            return 1f - v * d * d / (2f * e)
        }

        /**
         * How soon a load reported off is re-asserted. Measured on hardware: the
         * INITIAL LOAD_ON does not always land, and at a 1 s interval that lost
         * the first second of the sweep — the whole low-current end of the ramp,
         * which is where the fit most needs span. 400 ms catches it while still
         * being far longer than the 50 ms the device needs between commands.
         */
        private const val LOAD_REASSERT_MS = 400L

        /** Grace period to shed a stale protection trip during priming. */
        private const val FAULT_CLEAR_MS = 4000L

        /**
         * How long to wait for the load to start conducting before calling it a
         * wiring fault. Comfortably longer than several [LOAD_REASSERT_MS]
         * retries.
         */
        private const val ARM_MAX_MS = 3000L

        /**
         * Current that counts as "actually sinking". Well below the 50 mA floor
         * the ramp commands, so a load regulating in at the bottom of its range
         * still trips it, but clear of a zero reading with noise on it.
         */
        private const val ARM_MIN_CURRENT = 0.02f

        /**
         * Safe peak current for a given fuse rating and source voltage — the same
         * clamp the sweep applies, exposed so the setup screen can show the user
         * what their fuse actually permits before they start.
         */
        fun safeMaxCurrent(fuseRating: Float, voc: Float, safety: Float = 0.8f): Float {
            val v = if (voc > El15Protocol.MIN_VOLTAGE_V) voc else El15Protocol.MAX_VOLTAGE_V
            val powerCap = El15Protocol.MAX_POWER_W / v
            return minOf(
                fuseRating * safety, El15Protocol.MAX_CURRENT_A, powerCap, ABS_MAX_CURRENT,
            )
        }
    }
}
