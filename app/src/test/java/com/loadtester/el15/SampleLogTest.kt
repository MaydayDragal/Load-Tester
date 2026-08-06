package com.loadtester.el15

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The tiered datapoint log.
 *
 * Its job is to stay bounded however long a run goes without ever discarding
 * the run: each tier halves the resolution and takes half the remaining budget,
 * so the record count is capped while the curve still spans the whole test.
 */
class SampleLogTest {

    private fun fill(log: SampleLog, count: Int, stepMs: Long) {
        for (k in 0 until count) {
            log.add(k * stepMs, 12f, 1f, 25f, 0f, 0f)
        }
    }

    @Test
    fun `base interval decides what is stored`() {
        val log = SampleLog(baseIntervalMs = 1000, maxRecords = 1000)
        log.start()
        // Offer ten samples per second; only one per second should be kept.
        for (k in 0 until 100) log.add(k * 100L, 12f, 1f, 25f, 0f, 0f)
        assertEquals(10, log.count)
    }

    @Test
    fun `never exceeds the record budget`() {
        val log = SampleLog(baseIntervalMs = 100, maxRecords = 200)
        log.start()
        fill(log, 100_000, 100)
        assertTrue("count ${log.count} exceeded the 200 budget", log.count <= 200)
    }

    @Test
    fun `an over-long run still spans the whole test`() {
        val log = SampleLog(baseIntervalMs = 100, maxRecords = 200)
        log.start()
        val lastT = 99_999 * 100L
        fill(log, 100_000, 100)
        var first = -1L
        var last = -1L
        log.replay { r ->
            if (first < 0) first = r.tMs
            last = r.tMs
            true
        }
        assertEquals("the run must start at the beginning", 0L, first)
        assertTrue(
            "the log should reach near the end of the run (got $last of $lastT)",
            last > lastT * 0.5,
        )
    }

    @Test
    fun `resolution coarsens rather than dropping the early curve`() {
        val log = SampleLog(baseIntervalMs = 100, maxRecords = 200)
        log.start()
        fill(log, 100_000, 100)
        assertTrue(
            "interval should have coarsened past the base (got ${log.intervalMs})",
            log.intervalMs > 100,
        )
    }

    @Test
    fun `replay returns records in order`() {
        val log = SampleLog(baseIntervalMs = 10, maxRecords = 500)
        log.start()
        fill(log, 400, 10)
        var previous = -1L
        log.replay { r ->
            assertTrue("timestamps must be non-decreasing", r.tMs >= previous)
            previous = r.tMs
            true
        }
    }

    @Test
    fun `replay stops early when the consumer says so`() {
        val log = SampleLog(baseIntervalMs = 10, maxRecords = 500)
        log.start()
        fill(log, 100, 10)
        var seen = 0
        log.replay { seen++; seen < 5 }
        assertEquals(5, seen)
    }

    @Test
    fun `start drops the previous run`() {
        val log = SampleLog(baseIntervalMs = 10, maxRecords = 500)
        log.start()
        fill(log, 50, 10)
        assertTrue(log.count > 0)
        log.start()
        assertEquals("a new run must not inherit the last one's samples", 0, log.count)
        assertEquals("and must reset the tier schedule", 10L, log.intervalMs)
    }

    @Test
    fun `add before start is ignored`() {
        val log = SampleLog(baseIntervalMs = 10, maxRecords = 500)
        log.add(0, 12f, 1f, 25f, 0f, 0f)
        assertEquals(0, log.count)
    }

    @Test
    fun `payload fields round-trip`() {
        val log = SampleLog(baseIntervalMs = 10, maxRecords = 10)
        log.start()
        log.add(1234, 12.5f, 2.5f, 31.5f, 0.75f, 9.5f)
        var got: SampleLog.Rec? = null
        log.replay { got = it; true }
        assertEquals(1234L, got!!.tMs)
        assertEquals(12.5f, got!!.v, 1e-6f)
        assertEquals(2.5f, got!!.i, 1e-6f)
        assertEquals(31.5f, got!!.temp, 1e-6f)
        assertEquals(0.75f, got!!.aux0, 1e-6f)
        assertEquals(9.5f, got!!.aux1, 1e-6f)
    }
}
