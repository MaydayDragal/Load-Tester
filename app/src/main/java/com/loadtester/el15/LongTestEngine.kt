package com.loadtester.el15

import android.os.Handler
import android.os.Looper
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/**
 * Engine for long-running bench tests, all built on the same sampled loop:
 *
 *  - **Battery capacity** — CC discharge at a chemistry-appropriate current
 *    until the cutoff voltage; integrates Ah/Wh client-side and grades state
 *    of health against the rated capacity.
 *  - **Runtime at constant power** — CP until cutoff; reports runtime + energy.
 *  - **Step-load transient** — square-waves the current between two levels and
 *    measures worst-case droop and recovery time.
 *  - **OCP ramp** — slowly ramps current until the source's voltage collapses
 *    or protection trips; reports the trip point.
 *
 * Safety: every commanded current is clamped to min(safety·fuse, 12 A,
 * 150 W/Voc); a protection trip aborts (for OCP it *is* the result); the load
 * is commanded off on every exit path. Long time series are decimated to a
 * bounded point count so records stay small.
 */
class LongTestEngine(
    private val ble: El15Controller,
    private val callback: Callback,
) {
    interface Callback {
        fun onSessionProgress(text: String)
        fun onSessionComplete(record: SessionRecord)
        fun onSessionError(message: String)
    }

    /** Chemistry presets: per-cell cutoff + nominal voltage for cell counting. */
    data class Chemistry(val name: String, val cutoffPerCell: Float, val nominalPerCell: Float)

    var safetyFactor = 0.8f
    var pollIntervalMs = 500L

    val running: Boolean get() = state != State.IDLE
    val currentType: Int get() = type

    private enum class State { IDLE, PRIME, RUN }

    private val main = Handler(Looper.getMainLooper())
    private var state = State.IDLE
    private var type = 0
    private var fuse = 0f
    private var vOc = 0f
    private var params = LinkedHashMap<String, Float>()
    private var deviceLabel = ""
    private var startedMs = 0L
    private val points = ArrayList<SessionRecord.TimePoint>()
    private var decimation = 1
    private var sampleCounter = 0

    // accumulators
    private var ah = 0.0
    private var wh = 0.0
    private var lastSampleMs = 0L
    private var cutoffHits = 0

    // step-load state
    private var stepHigh = false
    private var stepFlips = 0
    private var stepNext = 0L
    private var droopMin = Float.MAX_VALUE
    private var vBeforeStep = 0f
    private var recoverySumMs = 0.0
    private var recoveryCount = 0
    private var awaitingRecovery = false
    private var stepFlipMs = 0L

    // OCP state
    private var rampI = 0f
    private var rampNext = 0L

    private val pending = ArrayList<Runnable>()

    fun start(type: Int, fuseA: Float, p: Map<String, Float>, device: String) {
        if (running) return
        this.type = type
        fuse = fuseA
        params = LinkedHashMap(p)
        deviceLabel = device
        vOc = 0f
        points.clear(); ah = 0.0; wh = 0.0; cutoffHits = 0
        sampleCounter = 0; decimation = 1
        stepHigh = false; stepFlips = 0; droopMin = Float.MAX_VALUE
        recoverySumMs = 0.0; recoveryCount = 0; awaitingRecovery = false
        rampI = 0f

        state = State.PRIME
        callback.onSessionProgress("Priming…")
        ble.setMode(if (type == SessionRecord.TYPE_RUNTIME) El15Protocol.MODE_CP else El15Protocol.MODE_CC)
        ble.setSetpoint(0f)
        ble.setLoad(false)
        schedule({ beginRun() }, maxOf(1200L, 2 * pollIntervalMs + 300))
    }

    fun stop() {
        if (!running) return
        finishSafely()
        state = State.IDLE
    }

    private fun safePeak(): Float =
        minOf(fuse * safetyFactor, El15Protocol.MAX_CURRENT_A,
            El15Protocol.MAX_POWER_W / max(vOc, El15Protocol.MIN_VOLTAGE_V))

    private fun beginRun() {
        if (!running) return
        if (vOc < El15Protocol.MIN_VOLTAGE_V) { abort("No reading from the load."); return }
        if (vOc > El15Protocol.MAX_VOLTAGE_V) { abort("Source exceeds the EL15's 60 V rating."); return }

        startedMs = System.currentTimeMillis()
        lastSampleMs = startedMs
        state = State.RUN
        when (type) {
            SessionRecord.TYPE_CAPACITY -> {
                val i = min(params["dischargeA"] ?: 1f, safePeak())
                params["dischargeA"] = i
                ble.setSetpoint(i)
                ble.setLoad(true)
            }
            SessionRecord.TYPE_RUNTIME -> {
                val pw = min(params["powerW"] ?: 10f, El15Protocol.MAX_POWER_W)
                    .coerceAtMost(vOc * safePeak())
                params["powerW"] = pw
                ble.setSetpoint(pw)
                ble.setLoad(true)
            }
            SessionRecord.TYPE_STEP -> {
                params["iLow"] = min(params["iLow"] ?: 0.2f, safePeak())
                params["iHigh"] = min(params["iHigh"] ?: 1f, safePeak())
                stepHigh = false
                stepNext = startedMs + (params["periodMs"] ?: 2000f).toLong() / 2
                ble.setSetpoint(params["iLow"]!!)
                ble.setLoad(true)
            }
            SessionRecord.TYPE_OCP -> {
                rampI = min(params["startA"] ?: 0.2f, safePeak())
                rampNext = startedMs + (params["dwellMs"] ?: 1500f).toLong()
                ble.setSetpoint(rampI)
                ble.setLoad(true)
            }
        }
    }

    fun onStatus(status: El15Status) {
        if (!running || !status.valid) return
        if (status.warning.isNotEmpty()) {
            if (type == SessionRecord.TYPE_OCP && state == State.RUN) {
                // Collapse/protection IS the OCP result.
                completeOcp(tripped = true)
            } else {
                abort("Load protection tripped (${status.warning}).")
            }
            return
        }
        when (state) {
            State.PRIME -> vOc = status.voltage
            State.RUN -> sampleAndDrive(status)
            else -> {}
        }
    }

    private fun sampleAndDrive(s: El15Status) {
        val now = System.currentTimeMillis()
        val dtH = (now - lastSampleMs) / 3_600_000.0
        lastSampleMs = now
        if (s.loadOn) {
            ah += s.current * dtH
            wh += s.power * dtH
        }

        // Decimated recording: cap memory by halving the sample rate whenever
        // the buffer fills (keeps the full time span at reduced resolution).
        sampleCounter++
        if (sampleCounter % decimation == 0) {
            points.add(SessionRecord.TimePoint(now - startedMs, s.voltage,
                s.current, s.temperature, s.fanSpeed))
            if (points.size >= MAX_POINTS) {
                val kept = points.filterIndexed { idx, _ -> idx % 2 == 0 }
                points.clear(); points.addAll(kept)
                decimation *= 2
            }
        }

        val elapsedS = (now - startedMs) / 1000
        when (type) {
            SessionRecord.TYPE_CAPACITY, SessionRecord.TYPE_RUNTIME -> {
                val cutoff = params["cutoffV"] ?: 0f
                callback.onSessionProgress(
                    "%s · %.2f V · %.3f Ah · %.2f Wh".format(
                        SessionRecord.fmtDuration(elapsedS), s.voltage, ah, wh))
                if (cutoff > 0f && s.voltage <= cutoff) {
                    if (++cutoffHits >= 3) { completeDischarge(); return }
                } else cutoffHits = 0
                // Hard time cap as a backstop (24 h).
                if (elapsedS > 86_400) completeDischarge()
            }
            SessionRecord.TYPE_STEP -> driveStep(s, now, elapsedS)
            SessionRecord.TYPE_OCP -> driveOcp(s, now)
        }
    }

    private fun driveStep(s: El15Status, now: Long, elapsedS: Long) {
        val periodMs = (params["periodMs"] ?: 2000f).toLong().coerceAtLeast(2 * pollIntervalMs)
        val cycles = (params["cycles"] ?: 10f).toInt()

        if (stepHigh) {
            droopMin = min(droopMin, s.voltage)
            if (awaitingRecovery && abs(s.voltage - vBeforeStep) <= 0.02f * vBeforeStep) {
                recoverySumMs += (now - stepFlipMs).toDouble()
                recoveryCount++
                awaitingRecovery = false
            }
        }
        callback.onSessionProgress(
            "Step cycle ${stepFlips / 2 + 1}/$cycles · %.2f V @ %.3f A".format(s.voltage, s.current))

        if (now >= stepNext) {
            stepHigh = !stepHigh
            stepFlips++
            stepFlipMs = now
            if (stepHigh) { vBeforeStep = s.voltage; awaitingRecovery = true }
            ble.setSetpoint(if (stepHigh) params["iHigh"]!! else params["iLow"]!!)
            stepNext = now + periodMs / 2
            if (stepFlips >= cycles * 2) { completeStep(); return }
        }
        if (elapsedS > 3600) completeStep()
    }

    private fun driveOcp(s: El15Status, now: Long) {
        val collapseFrac = params["collapseFrac"] ?: 0.7f
        callback.onSessionProgress("OCP ramp · %.2f A · %.2f V".format(rampI, s.voltage))
        if (s.loadOn && s.voltage < vOc * collapseFrac) {
            completeOcp(tripped = true)
            return
        }
        if (now >= rampNext) {
            val stepA = params["stepA"] ?: 0.1f
            val next = rampI + stepA
            if (next > safePeak()) { completeOcp(tripped = false); return }
            rampI = next
            ble.setSetpoint(rampI)
            rampNext = now + (params["dwellMs"] ?: 1500f).toLong()
        }
    }

    // ---- Completions ---------------------------------------------------------
    private fun buildRecord(metrics: Map<String, Float>): SessionRecord {
        val rec = SessionRecord(
            id = "s${System.currentTimeMillis()}",
            timestampMs = startedMs,
            type = type,
            deviceLabel = deviceLabel,
        )
        rec.params.putAll(params)
        rec.metrics.putAll(metrics)
        rec.points.addAll(points)
        return rec
    }

    private fun completeDischarge() {
        finishSafely(); state = State.IDLE
        val runtimeS = ((System.currentTimeMillis() - startedMs) / 1000).toFloat()
        val metrics = linkedMapOf(
            "capacityAh" to ah.toFloat(),
            "energyWh" to wh.toFloat(),
            "runtimeS" to runtimeS,
            "endV" to (points.lastOrNull()?.v ?: 0f),
            "startV" to vOc,
        )
        val rated = params["ratedAh"] ?: 0f
        if (type == SessionRecord.TYPE_CAPACITY && rated > 0f) {
            metrics["sohPct"] = (ah.toFloat() / rated * 100f).coerceIn(0f, 150f)
        }
        callback.onSessionComplete(buildRecord(metrics))
    }

    private fun completeStep() {
        finishSafely(); state = State.IDLE
        val droop = if (droopMin == Float.MAX_VALUE) 0f else max(0f, vOc - droopMin)
        val rec = if (recoveryCount > 0) (recoverySumMs / recoveryCount).toFloat() else 0f
        callback.onSessionComplete(buildRecord(linkedMapOf(
            "droopV" to droop,
            "vMin" to (if (droopMin == Float.MAX_VALUE) 0f else droopMin),
            "recoveryMs" to rec,
            "cycles" to (stepFlips / 2).toFloat(),
        )))
    }

    private fun completeOcp(tripped: Boolean) {
        finishSafely(); state = State.IDLE
        callback.onSessionComplete(buildRecord(linkedMapOf(
            "tripA" to (if (tripped) rampI else 0f),
            "maxA" to rampI,
            "startV" to vOc,
        )))
    }

    private fun abort(message: String) {
        finishSafely(); state = State.IDLE
        callback.onSessionError(message)
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

    companion object {
        private const val MAX_POINTS = 4000

        val CHEMISTRIES = listOf(
            Chemistry("Li-ion (3.7 V)", cutoffPerCell = 3.0f, nominalPerCell = 3.7f),
            Chemistry("LiFePO₄ (3.2 V)", cutoffPerCell = 2.5f, nominalPerCell = 3.2f),
            Chemistry("Lead-acid (2.0 V)", cutoffPerCell = 1.75f, nominalPerCell = 2.0f),
            Chemistry("NiMH (1.2 V)", cutoffPerCell = 1.0f, nominalPerCell = 1.2f),
        )

        /** Estimate the series cell count from open-circuit voltage. */
        fun estimateCells(voc: Float, chem: Chemistry): Int =
            (voc / chem.nominalPerCell + 0.35f).toInt().coerceIn(1, 30)
    }
}
