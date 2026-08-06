package com.loadtester.el15

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Inverting the load's corruption of a current reading.
 *
 * The numbers are real: reported values captured from an EL15 over BLE
 * (tools/el15_bench), with the true current taken from a local fit of the ramp
 * either side. What matters here is not how many are recovered but that
 * NOTHING is recovered wrongly — a fabricated current at an honest voltage is
 * indistinguishable from a measurement afterwards, so a refusal is always the
 * cheaper mistake.
 */
class RepairTest {

    private fun repair(reported: Float, predicted: Float) =
        ResistanceTest.repairOffTarget(reported, predicted)

    @Test
    fun `recovers a halved reading`() {
        // exponent's low bit dropped: 0.6022 A reported where the ramp was at 1.2067
        val got = repair(0.6022f, 1.2067f)
        assertNotNull(got)
        assertEquals(1.2044f, got!!, 1e-3f)
    }

    @Test
    fun `recovers a significand shifted two places`() {
        // 4v-3: 1.8032 A reported where the ramp was at 1.1993
        val got = repair(1.8032f, 1.1993f)
        assertNotNull(got)
        assertEquals(1.2008f, got!!, 1e-3f)
    }

    @Test
    fun `recovers a significand shifted one place`() {
        // 2v-1: 1.3954 A reported where the ramp was at 1.1977
        val got = repair(1.3954f, 1.1977f)
        assertNotNull(got)
        assertEquals(1.1977f, got!!, 2e-3f)
    }

    @Test
    fun `refuses a reading no inversion explains`() {
        // Captured, and matched by none of the three modes: 1.2817 A where the
        // ramp was at 1.2014. The nearest candidate is 1.141 - 60 mA out, so
        // repairing it would invent a plausible wrong current.
        assertNull(repair(1.2817f, 1.2014f))
        // Likewise 1.5729 A at 1.2030.
        assertNull(repair(1.5729f, 1.2030f))
    }

    @Test
    fun `refuses when the prediction cannot separate the candidates`() {
        // A prediction sitting between two inversions must not pick one. For
        // 1.40 A the candidates are 2.80 / 1.20 / 1.10; a prediction of 1.15 is
        // 50 mA from each, so nothing is identified.
        assertNull(repair(1.40f, 1.15f))
    }

    @Test
    fun `refuses when the ramp is nowhere near any candidate`() {
        // The load glitched somewhere the repair has no business guessing at.
        assertNull(repair(1.80f, 3.00f))
    }

    @Test
    fun `refuses nonsense inputs rather than returning one`() {
        assertNull(repair(0f, 1.2f))
        assertNull(repair(1.8f, 0f))
        assertNull(repair(-1f, 1.2f))
    }

    @Test
    fun `an uncorrupted reading maps to itself under no inversion`() {
        // Sanity: the inversions of a good 1.2 A reading are 2.4 / 1.1 / 1.05.
        // Predicting 1.2 leaves the nearest 100 mA away, so a good sample that
        // somehow reached the repair path is refused rather than altered.
        assertNull(repair(1.2f, 1.2f))
    }

    @Test
    fun `the tolerance is tight enough to reject the near miss that fabricates`() {
        // 1.2817 A at 1.2014 is the case that a 0.06 A tolerance gets WRONG:
        // on the bench that setting fabricated 8 currents across 166 events.
        // It must stay refused, and the boundary is worth pinning.
        assertNull(repair(1.2817f, 1.2014f))
        // 40 mA out: still refused.
        assertNull(repair(1.8032f, 1.2408f))
        // 20 mA out: recovered.
        assertNotNull(repair(1.8032f, 1.2208f))
    }
}
