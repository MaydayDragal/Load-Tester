package com.loadtester.el15

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Tests for the auto-calibration analysis math. */
class CalMathTest {

    private val m = SweepCalibrator.CalMath

    // ---- stabilizationTimeMs ------------------------------------------------

    @Test fun findsSettlingPointOfExponentialDecay() {
        // 12.6 V open circuit stepping to 11.6 V with a ~400 ms time constant.
        val trace = listOf(
            0L to 12.30f, 200L to 12.00f, 400L to 11.80f, 600L to 11.68f,
            800L to 11.62f, 1000L to 11.605f, 1200L to 11.601f, 1400L to 11.600f,
            1600L to 11.600f, 1800L to 11.600f,
        )
        val t = m.stabilizationTimeMs(trace, 12.6f)!!
        // Should land once readings are inside the band around 11.60, not at 0
        // and not at the very end.
        assertTrue("got $t", t in 600L..1200L)
    }

    @Test fun instantStepSettlesImmediately() {
        val trace = listOf(0L to 11.60f, 300L to 11.601f, 600L to 11.599f, 900L to 11.60f)
        assertEquals(0L, m.stabilizationTimeMs(trace, 12.6f))
    }

    @Test fun sparseTraceReturnsNull() {
        assertNull(m.stabilizationTimeMs(listOf(0L to 12f, 500L to 11f), 12.6f))
    }

    @Test fun neverSettlingTraceReturnsEnd() {
        // Monotonic drift that never enters the tail band until the end.
        val trace = (0..8).map { it * 300L to (12.6f - it * 0.2f) }
        val t = m.stabilizationTimeMs(trace, 12.6f)!!
        assertTrue(t >= trace[trace.size - 3].first)
    }

    // ---- sigma / sampleWindowMs ---------------------------------------------

    @Test fun sigmaOfConstantIsZero() {
        assertEquals(0f, m.sigma(listOf(5f, 5f, 5f)), 1e-6f)
        assertEquals(0f, m.sigma(listOf(5f)), 0f)
    }

    @Test fun quietSignalGetsMinimumWindow() {
        // σ well below target -> minimum 2 samples.
        assertEquals(2 * 500L + 250L, m.sampleWindowMs(0.0001f, 1.0f, 500L))
        assertEquals(2 * 500L + 250L, m.sampleWindowMs(0f, 1.0f, 500L))
    }

    @Test fun noisySignalGetsLargerWindowButCapped() {
        // σ = 10 mV against a 1 V drop (target 1 mV): n = 100 -> capped at 20.
        assertEquals(20 * 500L + 250L, m.sampleWindowMs(0.010f, 1.0f, 500L))
        // σ = 2 mV: n = 4.
        assertEquals(4 * 500L + 250L, m.sampleWindowMs(0.002f, 1.0f, 500L))
    }

    // ---- convergence / step choice -------------------------------------------

    @Test fun convergedDetectsStability() {
        assertTrue(m.converged(listOf(0.350f, 0.3505f)))
        assertFalse(m.converged(listOf(0.350f, 0.360f)))
        assertFalse(m.converged(listOf(0.350f)))
        assertFalse(m.converged(listOf(0f, 0f)))
    }

    @Test fun chooseStepsPicksTierAboveConvergence() {
        // Converged between sweep 1 and 2 (index 1) with good R² -> tier above = 12.
        val (steps, ok) = m.chooseSteps(
            listOf(0.350f, 0.3505f, 0.3503f), listOf(0.999f, 0.9995f, 0.9993f), listOf(5, 8, 12))
        assertTrue(ok)
        assertEquals(12, steps)
    }

    @Test fun chooseStepsAddsHeadroomWhenNotConverged() {
        val (steps, ok) = m.chooseSteps(
            listOf(0.30f, 0.34f, 0.38f), listOf(0.95f, 0.96f, 0.94f), listOf(5, 8, 12))
        assertFalse(ok)
        assertEquals(16, steps)
    }

    @Test fun chooseStepsRejectsConvergenceWithPoorFit() {
        // Values agree but R² is bad -> not converged.
        val (steps, ok) = m.chooseSteps(
            listOf(0.350f, 0.3501f), listOf(0.90f, 0.90f), listOf(5, 8, 12))
        assertFalse(ok)
        assertEquals(16, steps)
    }
}
