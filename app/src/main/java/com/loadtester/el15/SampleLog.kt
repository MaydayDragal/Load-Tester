package com.loadtester.el15

/**
 * Per-sample datapoint logs for the test engines, streamed into the CSV report
 * when a test ends. Kotlin port of the firmware's `sample_log.h`.
 *
 * The firmware buffers these in the ESP32's internal flash because a capacity
 * run is hours long and that board has ~37 KB of free heap and no PSRAM. A phone
 * has no such constraint, so this keeps them in RAM — but it keeps the firmware's
 * TIERED schedule regardless, for two reasons that still apply here: the CSV a
 * run produces stays a size a spreadsheet will actually plot, and a report saved
 * from the app is directly comparable to one saved by the board.
 *
 * Bounded without ever discarding the run: each log is written in TIERS. Tier 0
 * stores one sample per base interval until it has used half the record budget,
 * then tier 1 stores one per two intervals until it has used half of what is
 * left, and so on. Each tier therefore covers the same wall-clock span, the file
 * can never exceed the budget, and an arbitrarily long run still produces a
 * complete curve — just with coarser resolution towards the end. Every record
 * carries its own timestamp, so uneven spacing is self-describing in the CSV.
 *
 * TWO independent logs, one per test engine — never a shared one, because either
 * report streams from its log AT SAVE TIME: sharing would let a quick R-test wipe
 * the datapoints of a capacity run whose save is still pending.
 *
 * Main-thread confined, like the engines that write to it.
 */
class SampleLog(
    private val baseIntervalMs: Long,
    private val maxRecords: Int,
) {
    /**
     * One datapoint. Derivable values (power = V*I, mAh = Ah*1000) are computed
     * when the CSV is written rather than stored. The two payload floats mean
     * different things per engine:
     *  - batt:  aux0 = integrated capacity so far (Ah), aux1 = energy so far (Wh)
     *  - rtest: aux0 = commanded target (A),            aux1 = fan speed step
     */
    data class Rec(
        val tMs: Long,
        val v: Float,
        val i: Float,
        val temp: Float,
        val aux0: Float,
        val aux1: Float,
        /** rtest: spread of current across the level's samples | batt: unused. */
        val aux2: Float = 0f,
    )

    private val records = ArrayList<Rec>()
    private var active = false
    private var intervalMsCur = 0L
    private var nextAtMs = 0L

    /**
     * Records still allowed in the current tier. When it runs out the interval
     * doubles and the next tier gets half of whatever budget is left, so the
     * total is bounded by [maxRecords] however long the run goes while each tier
     * still covers the same wall-clock span.
     */
    private var tierLeft = 0

    /** Records stored so far. */
    val count: Int get() = records.size

    /** The interval the current tier is recording at. */
    val intervalMs: Long get() = intervalMsCur

    /** Begin a new run: drops any previous log and resets the tier schedule. */
    fun start(): Boolean {
        records.clear()
        records.ensureCapacity(minOf(maxRecords, 4096))
        active = true
        intervalMsCur = baseIntervalMs
        nextAtMs = 0
        tierLeft = maxOf(maxRecords / 2, 1)
        return true
    }

    /**
     * Offer a datapoint. Cheap to call on every status packet — the tier schedule
     * decides whether this one is actually stored, so the caller does not have to
     * rate-limit. [tMs] must be the run's elapsed milliseconds (for the capacity
     * test: active time, so the timeline stays monotonic across a pause).
     */
    fun add(
        tMs: Long, v: Float, i: Float, temp: Float,
        aux0: Float, aux1: Float, aux2: Float = 0f,
    ) {
        if (!active) return
        if (records.isNotEmpty() && tMs < nextAtMs) return
        records.add(Rec(tMs, v, i, temp, aux0, aux1, aux2))
        nextAtMs = tMs + intervalMsCur
        if (--tierLeft <= 0) nextTier()
    }

    /**
     * Halve the resolution and hand the next tier half of the remaining budget.
     * Once the budget is spent the log stops growing rather than evicting: the
     * early part of a curve is what the later part is measured against.
     */
    private fun nextTier() {
        val left = maxRecords - records.size
        if (left <= 0) { active = false; return }
        intervalMsCur *= 2
        tierLeft = maxOf(left / 2, 1)
    }

    /** Stream every stored record in order. [fn] returns false to stop early. */
    fun replay(fn: (Rec) -> Boolean) {
        for (r in records) if (!fn(r)) return
    }

    fun clear() {
        records.clear()
        records.trimToSize()
        active = false
    }

    companion object {
        /**
         * The instances. Budgets match the firmware's, which were bound by how
         * long an SD save could block its loop task (~16 s at 1 000 rows per two
         * seconds). Kept here because it is also about as many points as a
         * spreadsheet plots, and it keeps the two reports comparable.
         */

        /**
         * Capacity test: 2 s base interval — a discharge curve is a slow-moving
         * thing, and this keeps a typical 2 h test entirely in tier 0 while a
         * 12 h run still fits the budget.
         */
        val batt = SampleLog(2_000L, 8_000)

        /**
         * Resistance sweep: 25 ms base interval, BELOW the poll period, so tier 0
         * stores literally every status packet; a sweep of up to ~3 min is
         * captured at full rate and only the rare longer sweep coarsens.
         */
        val rtest = SampleLog(25L, 8_000)
    }
}
