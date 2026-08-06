package com.loadtester.el15

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

/**
 * The running least-squares fit behind the resistance sweep.
 *
 * [LeastSquares] is deliberately pure — no Android, no timers — so the maths the
 * whole R-Test rests on can be checked directly.
 */
class ResistanceFitTest {

    /** Feed a synthetic V = Voc - R*I line into a fresh accumulator. */
    private fun sweep(
        voc: Float, r: Float, from: Float, to: Float, n: Int,
        noise: (Int) -> Float = { 0f },
    ): LeastSquares {
        val lsq = LeastSquares()
        for (k in 0 until n) {
            val i = from + (to - from) * k / (n - 1).toFloat()
            lsq.add(i, voc - r * i + noise(k))
        }
        return lsq
    }

    @Test
    fun `recovers resistance and open-circuit voltage from a clean line`() {
        val f = sweep(voc = 12.6f, r = 0.35f, from = 0.1f, to = 4f, n = 60).fit()
        assertNotNull(f)
        assertEquals(0.35f, f!!.resistanceOhm, 1e-4f)
        assertEquals(12.6f, f.openCircuitVoltage, 1e-3f)
        assertEquals(1.0f, f.rSquared, 1e-4f)
        assertTrue(f.slopeNegative)
    }

    @Test
    fun `a clean line has essentially no uncertainty`() {
        val f = sweep(voc = 12.6f, r = 0.35f, from = 0.1f, to = 4f, n = 60).fit()!!
        assertTrue("stdErr should be ~0 on a noiseless line", f.resistanceStdErr < 1e-4f)
    }

    @Test
    fun `noise widens the uncertainty without moving the estimate much`() {
        // Deterministic zig-zag noise: same every run, so the test cannot flake.
        val noisy = sweep(voc = 12.6f, r = 0.35f, from = 0.1f, to = 4f, n = 60) { k ->
            if (k % 2 == 0) 0.01f else -0.01f
        }.fit()!!
        assertEquals(0.35f, noisy.resistanceOhm, 0.02f)
        assertTrue("noise must show up as uncertainty", noisy.resistanceStdErr > 1e-4f)
    }

    @Test
    fun `milliohm-scale resistance is resolved`() {
        // A 3 mohm circuit at 12 V: the case the sweep exists for, and the one
        // where R-squared-based reliability gating used to false-alarm.
        val f = sweep(voc = 12.0f, r = 0.003f, from = 0.05f, to = 10f, n = 300).fit()!!
        assertEquals(0.003f, f.resistanceOhm, 1e-5f)
    }

    @Test
    fun `too few samples yields no fit`() {
        val lsq = LeastSquares()
        repeat(LeastSquares.MIN_SAMPLES - 1) { k -> lsq.add(0.1f * k, 12.6f - 0.35f * 0.1f * k) }
        assertNull("a fit before MIN_SAMPLES would be meaningless", lsq.fit())
    }

    @Test
    fun `too narrow a current span yields no fit`() {
        // Every sample at essentially one current: the slope is undefined, and
        // reporting a number here is exactly the failure the span gate prevents.
        val lsq = LeastSquares()
        repeat(50) { lsq.add(2.0f, 11.9f) }
        assertNull(lsq.fit())
    }

    @Test
    fun `a rising line reports slopeNegative false and clamps resistance at zero`() {
        // Voltage rising with current is not a passive circuit; the engine must
        // not report a negative resistance.
        val lsq = LeastSquares()
        for (k in 0 until 40) {
            val i = 0.1f + k * 0.1f
            lsq.add(i, 10f + 0.05f * i)
        }
        val f = lsq.fit()!!
        assertFalse(f.slopeNegative)
        assertEquals(0f, f.resistanceOhm, 1e-6f)
    }

    @Test
    fun `bidirectional sweep cancels first-order drift`() {
        // The reason the ramp is a symmetric triangle: a source sagging linearly
        // in time biases an up-only sweep, but the up and down visits to each
        // current sit symmetrically about the midpoint and cancel.
        val voc = 12.6f
        val r = 0.35f
        val driftPerSample = 0.002f
        val n = 60

        val upOnly = LeastSquares()
        val triangle = LeastSquares()
        var t = 0
        for (k in 0 until n) {
            val i = 0.1f + (4f - 0.1f) * k / (n - 1).toFloat()
            upOnly.add(i, voc - r * i - driftPerSample * t)
            t++
        }
        t = 0
        for (k in 0 until n) {              // up
            val i = 0.1f + (4f - 0.1f) * k / (n - 1).toFloat()
            triangle.add(i, voc - r * i - driftPerSample * t); t++
        }
        for (k in n - 2 downTo 0) {         // and back down
            val i = 0.1f + (4f - 0.1f) * k / (n - 1).toFloat()
            triangle.add(i, voc - r * i - driftPerSample * t); t++
        }

        val upErr = abs(upOnly.fit()!!.resistanceOhm - r)
        val triErr = abs(triangle.fit()!!.resistanceOhm - r)
        assertTrue(
            "triangle ($triErr) should beat up-only ($upErr) under linear drift",
            triErr < upErr / 2f,
        )
    }

    @Test
    fun `accumulator tracks the measured ranges`() {
        val lsq = sweep(voc = 12.6f, r = 0.35f, from = 0.5f, to = 4f, n = 40)
        assertEquals(0.5f, lsq.minI, 1e-4f)
        assertEquals(4f, lsq.maxI, 1e-4f)
        assertEquals(3.5f, lsq.currentSpan, 1e-4f)
        // V falls as I rises, so maxV pairs with minI.
        assertEquals(12.6f - 0.35f * 0.5f, lsq.maxV, 1e-4f)
        assertEquals(12.6f - 0.35f * 4f, lsq.minV, 1e-4f)
    }

    @Test
    fun `reset clears the accumulator`() {
        val lsq = sweep(voc = 12.6f, r = 0.35f, from = 0.1f, to = 4f, n = 40)
        lsq.reset()
        assertEquals(0L, lsq.n)
        assertNull(lsq.fit())
    }
}
