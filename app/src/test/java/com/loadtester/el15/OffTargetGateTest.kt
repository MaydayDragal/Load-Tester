package com.loadtester.el15

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

/**
 * The window that decides whether a status frame is this circuit's operating
 * point or a bad reading from the load.
 *
 * The numbers below are not invented: they are the measured ones from
 * `RTEST_003` on a real EL15 (0.05 -> 4 A over 15 s, 250 packets), where the
 * load emitted three frames whose current field was wrong while their voltage
 * stayed exactly on the fitted line. The gate has to clear the worst HONEST
 * regulation lag in that run and still catch all three, or it is either useless
 * or it is throwing away data.
 */
class OffTargetGateTest {

    /** Commanded increment per 100 ms setpoint write on that 15 s / 4 A sweep. */
    private val step15s = (4.0f - 0.05f) / (15f / 2f / 0.1f)

    private fun rejected(measured: Float, target: Float, step: Float = step15s): Boolean =
        abs(measured - target) > ResistanceTest.offTargetLimit(target, step)

    @Test
    fun `the three glitch frames from the real sweep are rejected`() {
        // Ascent, twice (the device re-served the same frame), then the descent.
        assertTrue(rejected(measured = 1.7939f, target = 1.241f))
        assertTrue(rejected(measured = 1.7939f, target = 1.305f))
        assertTrue(rejected(measured = 0.6226f, target = 1.125f))
    }

    @Test
    fun `the worst honest regulation lag in that sweep is kept`() {
        // 0.135 A was the largest |measured - commanded| among the 247 good
        // frames. Nothing at or below it may be dropped.
        assertFalse(rejected(measured = 1.241f - 0.135f, target = 1.241f))
        assertFalse(rejected(measured = 3.500f - 0.135f, target = 3.500f))
        assertFalse(rejected(measured = 0.050f, target = 0.108f))
    }

    @Test
    fun `a fast sweep over a wide span still keeps its lag`() {
        // 5 s (the minimum) up to 12 A: the command moves 0.48 A per write, so a
        // reading two or three steps behind is the load doing its job, not a
        // glitch. The fixed floor alone would reject the whole ramp.
        val step = (12f - 0.05f) / (5f / 2f / 0.1f)
        assertFalse(rejected(measured = 2.0f - 3 * step, target = 2.0f, step = step))
        assertTrue("a wide gate is still a gate", rejected(measured = 0.2f, target = 6f, step = step))
    }

    @Test
    fun `the window scales with current so a high-current sweep is not over-gated`() {
        // At 10 A the fraction term governs: 20 % is 2 A of headroom.
        assertFalse(rejected(measured = 8.5f, target = 10f))
        assertTrue(rejected(measured = 7.5f, target = 10f))
    }

    @Test
    fun `a load reporting no current under a real command is rejected`() {
        // The dropout case the sweep must never fit: (0 A, Voc) is a leverage
        // point that drags the slope while saying nothing about it.
        assertTrue(rejected(measured = 0f, target = 2.5f))
    }

    @Test
    fun `the limit is never smaller than the floor`() {
        assertTrue(ResistanceTest.offTargetLimit(0f, 0f) >= 0.25f)
        assertTrue(ResistanceTest.offTargetLimit(0.05f, 0f) >= 0.25f)
    }
}
