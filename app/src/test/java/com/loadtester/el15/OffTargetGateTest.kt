package com.loadtester.el15

import org.junit.Assert.assertEquals
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

    // ---- the second trigger: sticking out past both neighbours -------------
    // The command comparison alone misses 4 of 15 corrupted readings, measured
    // over 12 real sweeps, because the load's mildest corruption at 1.20 A moves
    // the reading only 0.20 A against a 0.25 A window. A ramp is monotonic, so
    // this test catches what that one cannot without depending on the command.

    private fun out(v: Float, before: Float, after: Float) =
        ResistanceTest.sticksOut(v, before, after)

    @Test
    fun `a reading between its neighbours sticks out by nothing`() {
        assertEquals(0f, out(1.20f, 1.15f, 1.25f), 1e-6f)   // rising
        assertEquals(0f, out(1.20f, 1.25f, 1.15f), 1e-6f)   // falling
        assertEquals(0f, out(1.15f, 1.15f, 1.25f), 1e-6f)   // on a neighbour
    }

    @Test
    fun `a stale repeat sticks out by nothing`() {
        // The load re-serves the previous reading when polled faster than it
        // refreshes. That is not corruption and must never be treated as it.
        assertEquals(0f, out(1.1936f, 1.1936f, 1.3318f), 1e-6f)
        assertEquals(0f, out(0.6371f, 0.6371f, 0.4046f), 1e-6f)
    }

    @Test
    fun `lag however large does not stick out`() {
        // The command can run far ahead of the reading on a fast sweep; as long
        // as the readings themselves are ordered, none of them sticks out.
        assertEquals(0f, out(2.0f, 1.0f, 3.0f), 1e-6f)
    }

    @Test
    fun `the corrupted reading the command test misses is caught here`() {
        // Captured: 1.4024 A reported where the ramp was at ~1.19 - only 0.173 A
        // off the 1.230 A command, so the gate lets it through. It stands 0.20 A
        // clear of both neighbours.
        val e = out(1.4024f, 1.1900f, 1.2000f)
        assertTrue("should stand well clear of both neighbours, was $e", e > 0.15f)
    }

    @Test
    fun `the loud corruptions stick out by a mile`() {
        assertTrue(out(1.7849f, 1.1898f, 1.2090f) > 0.5f)    // 4v-3
        assertTrue(out(0.6022f, 1.2169f, 1.1965f) > 0.5f)    // v/2
    }

    @Test
    fun `honest scatter stays far below the threshold`() {
        // The 99th percentile of the honest excursion over 3511 real samples was
        // 0.0002 A. Even a hundred times that is nowhere near the 0.05 A trigger.
        assertTrue(out(1.2002f, 1.2000f, 1.2000f) < 0.05f)
        assertTrue(out(1.2200f, 1.2000f, 1.2000f) < 0.05f)
    }
}
