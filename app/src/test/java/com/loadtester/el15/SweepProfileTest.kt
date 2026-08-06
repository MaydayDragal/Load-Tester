package com.loadtester.el15

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

/**
 * The eased ramp profile behind the resistance sweep.
 *
 * A raw triangle has a step change in slope at switch-on, at the peak and at the
 * end; the load's CC regulator answers each with an overshoot. The easing
 * removes those corners, and these tests pin the three properties that make the
 * substitution safe: the ramp still covers the full span, it is still symmetric
 * about the peak (which is what cancels time drift in the fit), and the smoothing
 * costs only a few percent of extra peak slew.
 */
class SweepProfileTest {

    private fun ease(t: Float) = ResistanceTest.easeTriangle(t)

    @Test
    fun `covers the full range end to end`() {
        assertEquals(0f, ease(0f), 1e-6f)
        assertEquals(1f, ease(1f), 1e-5f)
    }

    @Test
    fun `is monotonic, so each current is reached exactly once per half-ramp`() {
        var previous = -1f
        var t = 0f
        while (t <= 1f) {
            val p = ease(t)
            assertTrue("profile must not go backwards at t=$t", p >= previous - 1e-6f)
            previous = p
            t += 0.001f
        }
    }

    @Test
    fun `clamps outside the unit interval`() {
        assertEquals(0f, ease(-0.5f), 1e-6f)
        assertEquals(1f, ease(1.7f), 1e-5f)
    }

    @Test
    fun `starts and ends with zero slope`() {
        // The whole point: no step change in commanded rate at the corners.
        val d = 1e-3f
        val startSlope = (ease(d) - ease(0f)) / d
        val endSlope = (ease(1f) - ease(1f - d)) / d
        val plateauSlope = (ease(0.5f + d) - ease(0.5f)) / d
        assertTrue("start slope $startSlope should be far below plateau $plateauSlope",
            startSlope < plateauSlope * 0.25f)
        assertTrue("end slope $endSlope should be far below plateau $plateauSlope",
            endSlope < plateauSlope * 0.25f)
    }

    @Test
    fun `slope is continuous everywhere`() {
        // No jump in rate between adjacent samples — a jump is precisely what
        // the regulator overshoots on.
        val step = 1e-3f
        var previousSlope = Float.NaN
        var t = 0f
        var worst = 0f
        while (t + step <= 1f) {
            val slope = (ease(t + step) - ease(t)) / step
            if (!previousSlope.isNaN()) worst = maxOf(worst, abs(slope - previousSlope))
            previousSlope = slope
            t += step
        }
        assertTrue("largest slope discontinuity was $worst", worst < 0.05f)
    }

    @Test
    fun `peak slope costs under 15 percent versus a linear ramp`() {
        // A linear ramp minimises peak rate of change for a given span and
        // duration, so any smoothing has to buy its soft corners somewhere. The
        // trapezoidal-velocity profile's bill is 1/(1 - EASE_FRAC).
        val step = 1e-4f
        var peak = 0f
        var t = 0f
        while (t + step <= 1f) {
            peak = maxOf(peak, (ease(t + step) - ease(t)) / step)
            t += step
        }
        assertTrue("peak slope $peak should stay near the linear rate of 1.0", peak < 1.15f)
        assertEquals("and should match 1/(1 - EASE_FRAC)",
            1f / (1f - ResistanceTest.EASE_FRAC), peak, 0.02f)
    }

    @Test
    fun `the full sweep is symmetric about its midpoint`() {
        // Drift cancellation depends on each current being visited at two times
        // equally spaced about the sweep midpoint. The easing is applied to the
        // triangle parameter, so that symmetry has to survive it.
        fun sweepAt(frac: Float): Float {
            val tri = if (frac < 0.5f) frac / 0.5f else 2f - frac / 0.5f
            return ease(tri)
        }
        var t = 0f
        while (t <= 0.5f) {
            assertEquals(
                "current at $t must match its mirror at ${1f - t}",
                sweepAt(t), sweepAt(1f - t), 1e-4f,
            )
            t += 0.005f
        }
    }

    @Test
    fun `reaches the peak exactly at the midpoint`() {
        assertEquals(1f, ease(1f), 1e-5f)
        // and nowhere before it
        assertTrue(ease(0.99f) < 1f)
    }
}
