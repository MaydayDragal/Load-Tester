package com.loadtester.el15

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.sqrt

/**
 * Verifies the pure V–I fit (resistance, open-circuit voltage, R², and the
 * slope's 1-sigma standard error) ported from the ESP firmware's accuracy pass.
 */
class ResistanceFitTest {

    private fun s(i: Float, v: Float) = CircuitResistanceTester.Sample(i, v, 25f, 0)

    @Test
    fun perfectLineRecoversRandVocWithZeroUncertainty() {
        // V = 12.6 − 0.35·I exactly.
        val voc = 12.6; val r = 0.35
        val pts = (1..8).map { k -> val i = k.toFloat(); s(i, (voc - r * i).toFloat()) }
        val f = CircuitResistanceTester.fit(pts)
        assertEquals(0.35f, f.resistanceOhm, 1e-4f)
        assertEquals(12.6f, f.openCircuitVoltage, 1e-3f)
        assertEquals(1.0f, f.rSquared, 1e-4f)
        assertTrue(f.slopeNegative)
        // A perfect line has zero residual → zero standard error.
        assertEquals(0f, f.resistanceStdErr, 1e-5f)
    }

    @Test
    fun standardErrorMatchesTheClosedForm() {
        // Symmetric residuals ±e about a clean line at I = 1,2,3,4 → hand-checkable.
        val r = 0.5; val voc = 10.0; val e = 0.02
        val currents = doubleArrayOf(1.0, 2.0, 3.0, 4.0)
        val resid = doubleArrayOf(+e, -e, +e, -e)
        val pts = currents.mapIndexed { k, i -> s(i.toFloat(), (voc - r * i + resid[k]).toFloat()) }
        val f = CircuitResistanceTester.fit(pts)

        // Closed form: with these residuals the fitted slope stays ≈ −0.5.
        val n = 4
        val meanI = currents.average()
        val sII = currents.sumOf { (it - meanI) * (it - meanI) }
        // Reconstruct V to compute SSE the same way the fit does.
        val vs = pts.map { it.voltage.toDouble() }
        val meanV = vs.average()
        var sIV = 0.0; var sVV = 0.0
        for (k in 0 until n) {
            val di = currents[k] - meanI; val dv = vs[k] - meanV
            sIV += di * dv; sVV += dv * dv
        }
        val slope = sIV / sII
        val sse = sVV - slope * sIV
        val expected = sqrt(sse / ((n - 2) * sII))
        assertEquals(expected.toFloat(), f.resistanceStdErr, 1e-5f)
        assertTrue("uncertainty must be non-trivial for noisy data", f.resistanceStdErr > 0f)
    }

    @Test
    fun bidirectionalAveragingCancelsLinearDrift() {
        // Simulate a linear-in-time drift added to a true 0.40 Ω line while the
        // ladder is swept up then down. Averaging the two visits per level must
        // recover the true resistance far better than a one-way ramp would.
        val rTrue = 0.40; val voc = 12.0
        val levels = (1..5).map { it * 1.0 }           // 1..5 A
        val order = levels.indices + (levels.indices.reversed().drop(1)) // up then down
        var t = 0
        val drift = 0.03 // volts of downward drift per visit (battery sag / heating)
        val acc = HashMap<Int, MutableList<Double>>()
        for (idx in order) {
            val i = levels[idx]
            val v = voc - rTrue * i - drift * t  // terminal sags progressively with time
            acc.getOrPut(idx) { mutableListOf() }.add(v)
            t++
        }
        val averaged = levels.indices.map { idx ->
            s(levels[idx].toFloat(), acc[idx]!!.average().toFloat())
        }
        val f = CircuitResistanceTester.fit(averaged)
        // Drift cancels: recovered R sits within 1 mΩ of the true 0.40 Ω.
        assertEquals(0.40f, f.resistanceOhm, 0.001f)
    }
}
