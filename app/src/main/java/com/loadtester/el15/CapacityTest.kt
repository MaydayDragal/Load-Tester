package com.loadtester.el15

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import kotlin.math.max
import kotlin.math.min

/**
 * Battery capacity discharge test. Kotlin port of the firmware's
 * `capacity_test.h` (see its CAPACITY_PLAN.md).
 *
 * Runs the load in CC mode and integrates Ah/Wh locally (trapezoid over sample
 * timestamps; CC status packets also carry temperature, which CAP-mode packets
 * do not). Discharges until the debounced minimum-voltage cutoff, a safety cap
 * (max time / max Ah), a protection trip, or a manual stop; then optionally
 * rests with the load off to capture the voltage rebound before reporting.
 *
 * Same architecture and safety discipline as [ResistanceTest]: timer-driven
 * state machine pumped by an internal ticker, fed live readings via [onStatus],
 * LOAD_OFF + setpoint 0 on every exit path. Every [stop] fires either
 * [Callback.onCapacityComplete] (any discharge data exists) or
 * [Callback.onCapacityError], so the UI never has to guess the engine's state.
 *
 * Main-thread confined.
 */
class CapacityTest(
    private val ble: El15Controller,
    private val callback: Callback,
) {
    data class Result(
        val capacityAh: Float,
        val energyWh: Float,
        val durationS: Long,
        val startV: Float,
        val endV: Float,
        val reboundV: Float,
        val avgV: Float,
        val avgI: Float,
        val minTemp: Float,
        val maxTemp: Float,
        val maxFan: Int,
        val cutoffV: Float,
        val currentA: Float,
        val stopReason: String,
        // Rated-capacity derived figures. ratedAh == 0 means the user did not
        // enter a rating, and sohPct/cRate are then meaningless and not shown.
        val ratedAh: Float,
        /** measured / rated x 100 — state of health. */
        val sohPct: Float,
        /** Discharge current expressed in C. */
        val cRate: Float,
        /** Total time spent paused (excluded from [durationS]). */
        val pausedS: Long,
        // Battery-model figures. All are 0 / -1 when the chemistry carries no
        // voltage curve (Custom) or the model never established itself.
        /**
         * Measured from the switch-on sag, so it covers everything between the
         * sense point and the cells: with probe compensation active (4-wire, or a
         * 2-wire lead tare) the leads are already out of the reading and this is
         * the pack alone; without it, it is pack + contacts + leads.
         */
        val internalResistanceOhm: Float,
        /** State of charge when the discharge began, or -1. */
        val startSocPct: Float,
        /** ... and when it stopped, or -1. */
        val endSocPct: Float,
        /**
         * Full-pack capacity implied by this run: Ah drawn per unit of state of
         * charge travelled, extrapolated to the whole 0-100 % range. Meaningful
         * even for a PARTIAL discharge, which [capacityAh] is not.
         */
        val impliedFullAh: Float,
        /**
         * Probe wiring in force during the run (snapshotted at start), so the
         * saved report labels what its voltages mean even if Settings change
         * before a (re)save.
         */
        val fourWire: Boolean,
        val tareOhm: Float,
        // The safety caps this run actually armed (see resolveCaps). Recorded so
        // a run that ended on a cap is traceable to the cap in the report,
        // instead of looking like a battery that simply stopped there.
        val capAh: Float,
        val capDurationS: Long,
        val chemistry: Int,
        val cells: Int,
    )

    interface Callback {
        /** [phase]: 1 = discharging, 2 = resting, 3 = paused. */
        fun onCapacityProgress(
            v: Float, i: Float, ah: Float, wh: Float, temp: Float,
            elapsedS: Long, phase: Int,
        )

        fun onCapacityComplete(result: Result)
        fun onCapacityError(message: String)

        /**
         * Pause/resume transitions, so the UI can explain itself. [reason] is set
         * while paused and null on resume.
         */
        fun onCapacityPause(paused: Boolean, reason: String?) {}
    }

    // ---- Configuration — set before start() ----------------------------------
    /** Automatic minimum-voltage stop point. */
    var cutoffV: Float = 0f

    /** Requested discharge current. */
    var dischargeA: Float = 0f

    /** Optional nameplate capacity (0 = unknown). */
    var ratedAh: Float = 0f

    /**
     * Safety caps. Both default to 0, meaning "work it out from the pack at
     * [start]" — see [resolveCaps]. Set either non-zero only to override.
     *
     * What these are FOR: bounding a runaway — a discharge that keeps going
     * because the cutoff never fired (a sense lead fallen off, a stuck voltage
     * reading, a cutoff set below what the pack can reach). They are not meant to
     * end a healthy test.
     *
     * A FIXED number does exactly that, though, and did: a flat 50 Ah stopped a
     * 92 Ah pack at 50 Ah — hours short of its cutoff — and reported "Capacity cap
     * reached" as though that were the measurement. The flat 12 h was the same
     * trap one step behind it: a 0.05C discharge takes 20 h BY DEFINITION, and
     * 0.05C is the standard rate for exactly the lead-acid packs this bench uses,
     * so no such test could ever have finished. Caps that scale with the pack stay
     * guards instead of becoming the answer.
     */
    var maxDurationS: Long = 0   // 0 = derive; counts ACTIVE time only
    var maxAh: Float = 0f        // 0 = derive

    /** Post-cutoff rebound window in seconds (0 = none). */
    var restS: Long = 60

    /**
     * Battery model for the time-remaining estimate: an index into
     * [BatteryModel.CHEMS] and the pack's series cell count. A chemistry with no
     * voltage curve (Custom), or a cell count of 0, leaves the engine on the
     * rated-capacity estimate — nothing else changes.
     */
    var chemistry: Int = -1
    var cells: Int = 0

    /**
     * Probe-wiring snapshot for the report, set by the caller at start time from
     * the app-wide setting. The engine itself never uses it — compensation is
     * applied upstream in [DeviceCore.compensateProbe] — it is only carried into
     * [Result] so the CSV describes the run as it was actually recorded.
     */
    var fourWire: Boolean = false
    var tareOhm: Float = 0f

    val running: Boolean get() = state != State.IDLE
    val isPaused: Boolean get() = state == State.PAUSED
    var pauseReason: String = ""
        private set

    private enum class State { IDLE, PRIMING, DISCHARGING, PAUSED, RESTING }

    private val main = Handler(Looper.getMainLooper())
    private var state = State.IDLE

    private var tStart = 0L
    private var endMs = 0L
    private var lastMs = 0L
    private var pausedMs = 0L
    private var pauseStartMs = 0L
    private var primeDoneAtMs = 0L
    private var primeDeadlineMs = 0L
    private var restDoneAtMs = 0L
    private var primeCount = 0

    private var ah = 0f
    private var wh = 0f
    private var sumVdt = 0f
    private var sumDt = 0f
    private var vOc = 0f
    private var vNow = 0f
    private var startV = 0f
    private var endV = 0f
    private var reboundV = 0f
    private var effA = 0f
    private var minT = 0f
    private var maxT = 0f
    private var lastTemp = 0f
    private var fanMax = 0
    private var below = 0
    private var haveSample = false
    private var logging = false
    private var stopReason = ""

    /** Safety caps resolved at [finishPriming]; 0 until then, and checked for >0 at
     *  both enforcement sites so a cap can never fire before it has been armed. */
    private var capAh = 0f
    private var capS = 0L

    // Battery-model state (see updateModel).
    private var rInt = 0f
    private var rIntSum = 0f
    private var rIntN = 0
    private var rIntDone = false
    private var socNow = 0f
    private var socStart = 0f
    private var socCut = 0f
    private var socValid = false
    private var ahAtSocStart = 0f
    private var qLearned = 0f
    private var lastSocMs = 0L

    // ---- Live readouts for the UI --------------------------------------------
    /** Seconds of ACTIVE discharge so far (paused time excluded). */
    fun elapsedS(): Long {
        if (state == State.IDLE || tStart == 0L) return 0
        val end = if (state == State.RESTING) endMs else now()
        val paused = pausedMs + if (state == State.PAUSED) now() - pauseStartMs else 0L
        return (end - tStart - paused) / 1000
    }

    /**
     * Estimated seconds of discharge left. 0 = no estimate available.
     *
     * Preferred path (a chemistry with a voltage curve): how much state of charge
     * is still to be travelled before the CUTOFF is reached, scaled by the pack's
     * capacity. That capacity is learned from this very run — Ah drawn per unit of
     * SoC travelled — so the estimate stops depending on the curve's absolute
     * calibration within the first few minutes, and works with no nameplate
     * rating entered at all.
     *
     * Fallback (Custom chemistry, unknown cell count, or before the model has
     * established itself): the rated-capacity estimate. It assumes the pack
     * started full and that the run ends at the rating rather than at the cutoff,
     * which is exactly what the curve path exists to fix.
     */
    fun remainingS(): Long {
        if (state != State.DISCHARGING || effA <= 0.001f) return 0
        if (socValid) {
            val q = if (qLearned > 0f) qLearned else ratedAh
            if (q > 0f) {
                val left = (socNow - socCut) * q
                if (left <= 0f) return 0
                return (left / effA * 3600f).toLong()
            }
        }
        if (ratedAh <= 0f) return 0
        val left = ratedAh - ah
        if (left <= 0f) return 0
        return (left / effA * 3600f).toLong()
    }

    /**
     * State of charge as a percentage, or -1 until the model has measured the
     * pack's internal resistance and taken its first reading (a few seconds of
     * discharge).
     */
    fun socPct(): Float = if (socValid) socNow * 100f else -1f

    /**
     * True when [remainingS] is coming from the discharge curve rather than from
     * the nameplate rating — the UI says which, because they are not equally
     * trustworthy and the user should know what they are looking at.
     */
    fun etaFromCurve(): Boolean = socValid && (qLearned > 0f || ratedAh > 0f)

    /**
     * Pack + contact + lead resistance measured from the switch-on sag (0 = not
     * measured yet). A useful number in its own right: it is most of what
     * separates a tired pack from a healthy one at the same capacity.
     */
    fun internalResistanceOhm(): Float = if (rIntDone) rInt else 0f

    /** Live integrated capacity/energy, for the UI while a run is in flight. */
    val capacityAh: Float get() = ah
    val energyWh: Float get() = wh

    // ---- Lifecycle -----------------------------------------------------------
    fun start() {
        if (running) return
        if (cutoffV <= 0.05f || dischargeA <= 0.005f) {
            callback.onCapacityError("Set a cutoff voltage and discharge current first.")
            return
        }
        ah = 0f; wh = 0f; sumVdt = 0f; sumDt = 0f
        vOc = 0f; vNow = 0f; startV = 0f; endV = 0f; reboundV = 0f
        minT = 0f; maxT = 0f; fanMax = 0
        below = 0; lastMs = 0; haveSample = false
        stopReason = ""
        pausedMs = 0; pauseStartMs = 0; pauseReason = ""
        tStart = 0; endMs = 0
        capAh = 0f; capS = 0     // armed by resolveCaps() once the current is known
        rInt = 0f; rIntSum = 0f; rIntN = 0; rIntDone = false
        socNow = 0f; socStart = 0f; socCut = 0f; socValid = false
        ahAtSocStart = 0f; qLearned = 0f; lastSocMs = 0
        logging = SampleLog.batt.start()

        state = State.PRIMING
        ble.setMode(El15Protocol.MODE_CC)
        ble.setSetpoint(0f)
        ble.setLoad(false)
        // Priming waits for READINGS, not for a stopwatch: the window is
        // wall-clock while the readings depend on the link, and starting a test
        // is a busy moment. The minimum window is PRIME_MIN_MS, but the test only
        // gives up once it has actually waited PRIME_MAX_MS without a single
        // packet — and then says so in those words.
        primeCount = 0
        primeDoneAtMs = now() + PRIME_MIN_MS
        primeDeadlineMs = now() + PRIME_MAX_MS
        startTicker()
    }

    /**
     * Manual/external stop. Mid-discharge (or rest, or paused) the data collected
     * so far is a valid partial result, so it completes with [reason]; during
     * priming it cancels via [Callback.onCapacityError]. Either way the UI always
     * gets exactly one callback.
     */
    fun stop(reason: String = "Stopped manually") {
        if (!running) return
        when (state) {
            State.DISCHARGING, State.PAUSED -> {
                // Close out the paused span and rejoin the normal path, so
                // elapsedS() below counts it exactly once.
                if (state == State.PAUSED) {
                    pausedMs += now() - pauseStartMs
                    state = State.DISCHARGING
                }
                stopReason = reason
                endV = vNow
                reboundV = vNow
                endMs = now()
                finishSafely()
                complete()
            }
            State.RESTING -> { finishSafely(); complete() }
            else -> {   // PRIMING
                finishSafely()
                state = State.IDLE
                callback.onCapacityError("Cancelled.")
            }
        }
    }

    /**
     * Suspend the discharge WITHOUT ending the test: the load goes off and the
     * clock stops, but every accumulator (Ah, Wh, min/max, the datapoint log) is
     * kept so [resume] carries straight on. This is what a dropped BLE link or a
     * low phone battery should do — those are reasons to stop drawing current,
     * not reasons to throw away hours of measurement.
     *
     * Only a live discharge can pause; returns false otherwise so the caller can
     * fall back to [stop]. Priming has nothing worth keeping and resting is
     * already load-off and nearly over.
     */
    fun pause(reason: String): Boolean {
        if (state != State.DISCHARGING) return false
        pauseStartMs = now()
        pauseReason = reason
        state = State.PAUSED
        finishSafelyKeepTicking()   // LOAD OFF + setpoint 0 — the point of pausing
        callback.onCapacityPause(true, reason)
        callback.onCapacityProgress(vNow, 0f, ah, wh, lastTemp, elapsedS(), 3)
        return true
    }

    /** Resume a paused discharge. Returns false if there was nothing to resume. */
    fun resume(): Boolean {
        if (state != State.PAUSED) return false
        pausedMs += now() - pauseStartMs
        pauseReason = ""
        // Drop the integration anchor: no Ah may be accrued across the paused
        // gap, and the load needs a moment to re-regulate before its readings
        // count.
        lastMs = 0
        below = 0
        state = State.DISCHARGING
        ble.setSetpoint(effA)
        ble.setLoad(true)
        callback.onCapacityPause(false, null)
        return true
    }

    // ---- Live readings -------------------------------------------------------
    fun onStatus(status: El15Status) {
        if (!running || !status.valid) return
        vNow = status.voltage
        lastTemp = status.temperature

        when (state) {
            State.PRIMING -> { vOc = status.voltage; primeCount++; return }
            State.PAUSED -> {
                // Still worth showing the (now unloaded) voltage recovering, but
                // nothing is integrated and the elapsed clock is frozen.
                callback.onCapacityProgress(
                    status.voltage, status.current, ah, wh, status.temperature,
                    elapsedS(), 3,
                )
                return
            }
            State.RESTING -> {
                reboundV = status.voltage
                callback.onCapacityProgress(
                    status.voltage, status.current, ah, wh, status.temperature,
                    elapsedS(), 2,
                )
                return
            }
            State.IDLE -> return
            State.DISCHARGING -> { /* fall through */ }
        }

        if (status.warning.isNotEmpty()) {
            stopReason = "Protection tripped"
            enterRest()
            return
        }
        val nowMs = now()
        if (lastMs != 0L) {
            val dtH = (nowMs - lastMs) / 3_600_000f
            if (dtH > 0f && dtH < 0.01f) {   // ignore >36 s gaps (link stall / glitch)
                ah += status.current * dtH
                wh += status.voltage * status.current * dtH
                sumVdt += status.voltage * dtH
                sumDt += dtH
            }
        }
        lastMs = nowMs
        if (!haveSample) {
            haveSample = true; minT = status.temperature; maxT = status.temperature
        } else {
            minT = min(minT, status.temperature); maxT = max(maxT, status.temperature)
        }
        fanMax = max(fanMax, status.fanSpeed)
        updateModel(status, nowMs)

        val el = elapsedS()
        // Offer every reading to the datapoint log; its tier schedule decides
        // which are actually stored, so this stays cheap at a fast poll rate.
        if (logging) {
            SampleLog.batt.add(el * 1000L, status.voltage, status.current,
                status.temperature, ah, wh)
        }

        callback.onCapacityProgress(
            status.voltage, status.current, ah, wh, status.temperature, el, 1,
        )

        // Debounced automatic cutoff: three consecutive samples at/below the stop
        // point, or a single sample well below it (fail-safe against noise).
        if (status.voltage <= cutoffV) below++ else below = 0
        if (below >= 3 || status.voltage < cutoffV - 0.3f) {
            stopReason = "Cutoff voltage reached"
            enterRest()
            return
        }
        if (capAh > 0f && ah >= capAh) {
            stopReason = "Capacity cap reached"
            enterRest()
            return
        }
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
        // The cap counts ACTIVE discharge time, so a long pause can't end the
        // test by itself — but the total energy budget it guards is unchanged.
        if (state == State.DISCHARGING && capS > 0 && elapsedS() >= capS) {
            stopReason = "Max duration reached"
            enterRest()
            return
        }
        if (state == State.PRIMING && now() >= primeDoneAtMs) {
            // Not one reading yet, and still inside the patience window: keep
            // waiting rather than blaming the battery for a stalled link.
            if (primeCount == 0 && now() < primeDeadlineMs) {
                primeDoneAtMs = now() + 250
                return
            }
            finishPriming()
            return
        }
        if (state == State.RESTING && now() >= restDoneAtMs) complete()
    }

    // ---- Battery model -------------------------------------------------------
    private fun haveCurve(): Boolean = BatteryModel.hasCurve(chemistry) && cells > 0

    /**
     * Internal resistance, state of charge, and the pack capacity implied by the
     * two together. Called once per discharging sample.
     *
     * The voltage the load reports is the cells' open-circuit voltage minus I*R of
     * the pack, its contacts and the test leads, so it cannot be looked up on a
     * RESTED voltage curve directly — at 2 A through 150 mohm that is 0.3 V, which
     * on Li-ion is most of the difference between 40 % and 70 % charged. Priming
     * gives the open-circuit voltage and the first seconds of discharge give the
     * loaded voltage at a known current, so R falls straight out of the two.
     *
     * Using the SAME R at both ends is what makes the estimate self-consistent:
     * the engine cuts on the LOADED voltage, so the state of charge the run will
     * actually stop at is the one at (cutoff + I*R), and any lead resistance in R
     * therefore shifts both the current reading and the target together instead of
     * biasing the time between them.
     */
    private fun updateModel(s: El15Status, nowMs: Long) {
        if (!haveCurve()) return

        if (!rIntDone) {
            // Active discharge time, not wall clock: a pause in the first seconds
            // would otherwise run the window out while the load was off.
            val since = nowMs - tStart - pausedMs
            // Only sample once the load is actually regulating at the commanded
            // current — a reading taken while it is still ramping in would divide
            // a real sag by a fraction of the real current and overstate R badly.
            if (since >= IR_WINDOW_START_MS && s.current > effA * 0.8f) {
                val r = (startV - s.voltage) / s.current
                if (r > 0f && r < IR_MAX_OHM) { rIntSum += r; rIntN++ }
            }
            // Close the window on TIME AND a quorum of samples. Requiring both
            // means a run whose first seconds were lost (a pause, or a load slow
            // to regulate) measures R late rather than not at all — and a load
            // that never reaches the commanded current never fabricates one,
            // which correctly leaves the estimate on its rated-capacity fallback.
            if (since < IR_WINDOW_END_MS || rIntN < IR_MIN_SAMPLES) return
            rInt = rIntSum / rIntN
            rIntDone = true
            socCut = BatteryModel.socFromOcv(chemistry, (cutoffV + effA * rInt) / cells)
            if (socCut < 0f) socCut = 0f
            return
        }

        val soc = BatteryModel.socFromOcv(chemistry, (s.voltage + s.current * rInt) / cells)
        if (soc < 0f) return
        if (!socValid) {
            socNow = soc; socStart = soc
            ahAtSocStart = ah   // the first few seconds of Ah predate this anchor
            socValid = true
            lastSocMs = nowMs
        } else {
            // Time-constant filter rather than a fixed per-sample coefficient:
            // the sample rate is a user setting, and a fixed coefficient would
            // make the filter far slower at the bottom of that range. A gap (link
            // stall, a resume after a pause) is skipped rather than integrated.
            val dt = (nowMs - lastSocMs) / 1000f
            lastSocMs = nowMs
            if (dt > 0f && dt < 10f) socNow += (soc - socNow) * (dt / (dt + SOC_TAU_S))
        }

        // Learn the pack's real capacity: Ah out per unit of state of charge
        // travelled. This is the step that turns "trust the curve" into a
        // measurement — after the first 10 % of travel the curve only has to get
        // the SHAPE right, not the absolute calibration, and an estimate becomes
        // possible with no nameplate rating entered at all.
        val travelled = socStart - socNow
        if (travelled > SOC_LEARN_MIN) {
            val q = (ah - ahAtSocStart) / travelled
            // A nameplate rating is not authoritative — measuring how far it is
            // out is the point of the test — but it does bound what is a
            // plausible reading versus arithmetic on noise.
            var sane = q > 0.001f && q < 2000f
            if (sane && ratedAh > 0f) sane = q > ratedAh * 0.05f && q < ratedAh * 5f
            if (sane) qLearned = q
        }
    }

    /**
     * Turn the configured caps (or 0 = auto) into the concrete limits this run
     * will enforce. Called once the commanded current is known.
     */
    private fun resolveCaps() {
        // Capacity: twice the nameplate. A pack does not deliver double its
        // rating, so reaching that means the measurement is wrong rather than the
        // battery being remarkable — which is exactly the runaway worth stopping.
        // The 10 Ah floor keeps a small pack's cap from sitting so close to its
        // real capacity that ordinary over-delivery trips it. With no nameplate
        // entered there is nothing to scale from, so fall back to a ceiling far
        // past any pack this 150 W load will realistically discharge; the time
        // cap below is the meaningful guard in that case.
        capAh = when {
            maxAh > 0f -> maxAh
            ratedAh > 0f -> max(ratedAh * 2f, 10f)
            else -> 1000f
        }

        capS = if (maxDurationS > 0) {
            maxDurationS
        } else {
            // Time: 2.5x the full discharge this current implies, so a slow
            // C-rate gets the hours it genuinely needs. Clamped at both ends —
            // neither a huge pack nor a trickle current may arm an effectively
            // unbounded run, and a tiny pack still gets a couple of hours of
            // headroom.
            var hours = 48f   // unknown pack: generous, but not forever
            if (ratedAh > 0f && effA > 0.001f) hours = ratedAh / effA * 2.5f
            hours = hours.coerceIn(2f, 72f)
            (hours * 3600f).toLong()
        }
    }

    // ---- State transitions ---------------------------------------------------
    private fun finishPriming() {
        val voc = vOc
        // "No telemetry" and "the pack is flat" are different faults with
        // different fixes, and running them together as "No reading, or voltage
        // below the minimum" leaves the user to guess whether to check the BLE
        // link or the battery. Say which one it is.
        if (primeCount == 0) {
            abort("No telemetry from the load - check the connection, then retry.")
            return
        }
        if (voc < El15Protocol.MIN_VOLTAGE_V) {
            abort("Battery voltage below the minimum. Test aborted."); return
        }
        if (voc > El15Protocol.MAX_VOLTAGE_V) {
            abort("Source above the EL15's 60 V rating. Test aborted."); return
        }
        if (voc <= cutoffV + 0.2f) {
            abort("Battery is already at or below the cutoff voltage."); return
        }
        effA = minOf(dischargeA, El15Protocol.MAX_CURRENT_A, El15Protocol.MAX_POWER_W / voc)
        if (effA < 0.01f) { abort("Safe discharge current too low."); return }
        // Arm the safety caps now: this is the first point where the current the
        // load will ACTUALLY draw is known, and the time cap is derived from it.
        resolveCaps()
        startV = voc
        below = 0
        lastMs = 0
        state = State.DISCHARGING
        tStart = now()
        ble.setSetpoint(effA)
        ble.setLoad(true)
    }

    private fun enterRest() {
        finishSafelyKeepTicking()
        endV = vNow
        reboundV = vNow
        endMs = now()
        if (restS <= 0) { complete(); return }
        state = State.RESTING
        restDoneAtMs = now() + restS * 1000L
    }

    private fun complete() {
        finishSafely()
        val activeS = elapsedS()
        val pausedS = pausedMs / 1000
        state = State.IDLE
        callback.onCapacityComplete(
            Result(
                capacityAh = ah,
                energyWh = wh,
                durationS = activeS,
                startV = startV,
                endV = endV,
                reboundV = reboundV,
                avgV = if (sumDt > 1e-9f) sumVdt / sumDt else 0f,
                avgI = if (sumDt > 1e-9f) ah / sumDt else 0f,
                minTemp = if (haveSample) minT else 0f,
                maxTemp = if (haveSample) maxT else 0f,
                maxFan = fanMax,
                cutoffV = cutoffV,
                currentA = effA,
                stopReason = stopReason.ifEmpty { "Completed" },
                ratedAh = ratedAh,
                sohPct = if (ratedAh > 0f) ah / ratedAh * 100f else 0f,
                cRate = if (ratedAh > 0f && effA > 0f) effA / ratedAh else 0f,
                pausedS = pausedS,
                internalResistanceOhm = if (rIntDone) rInt else 0f,
                startSocPct = if (socValid) socStart * 100f else -1f,
                endSocPct = if (socValid) socNow * 100f else -1f,
                // Only claim an implied full capacity if one was actually
                // learned. A run stopped after 2 % of travel has not measured
                // anything worth extrapolating.
                impliedFullAh = qLearned,
                fourWire = fourWire,
                tareOhm = if (fourWire) 0f else tareOhm,
                capAh = capAh,
                capDurationS = capS,
                chemistry = chemistry,
                cells = cells,
            )
        )
    }

    private fun abort(message: String) {
        finishSafely()
        state = State.IDLE
        callback.onCapacityError(message)
    }

    /** Load OFF + setpoint 0, and stop the ticker: the engine is done. */
    private fun finishSafely() {
        main.removeCallbacks(ticker)
        ble.setLoad(false)
        ble.setSetpoint(0f)
    }

    /**
     * Load OFF + setpoint 0 while the state machine keeps running — used by
     * pause and by the rest window, both of which still need the ticker to fire.
     */
    private fun finishSafelyKeepTicking() {
        ble.setLoad(false)
        ble.setSetpoint(0f)
    }

    private fun now(): Long = SystemClock.elapsedRealtime()

    companion object {
        private const val TICK_MS = 250L

        /**
         * Priming: the shortest settling window, and the longest we will wait for
         * the first telemetry packet before calling the link dead.
         */
        private const val PRIME_MIN_MS = 1500L
        private const val PRIME_MAX_MS = 8000L

        /**
         * Window over which the switch-on sag is averaged into an
         * internal-resistance figure. It starts late enough for the load to be
         * regulating at the commanded current and ends before the pack has
         * meaningfully discharged.
         */
        private const val IR_WINDOW_START_MS = 1500L
        private const val IR_WINDOW_END_MS = 5000L

        /** Quorum before R is trusted. */
        private const val IR_MIN_SAMPLES = 3

        /** Beyond this it is a bad reading, not a pack. */
        private const val IR_MAX_OHM = 20f

        /** State-of-charge filter time constant, seconds. */
        private const val SOC_TAU_S = 30f

        /** SoC travel before capacity is learned. */
        private const val SOC_LEARN_MIN = 0.10f
    }
}
