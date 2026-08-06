package com.loadtester.el15

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The chemistry curves behind the capacity test's time estimate.
 *
 * These tables are what let the engine read a state of charge off a voltage
 * instead of coulomb-counting against a nameplate rating, so their invariants
 * matter: a non-monotonic curve would make the lookup ambiguous, and a cell
 * ceiling that allows a pack over 60 V would offer a test that cannot be run.
 */
class BatteryModelTest {

    @Test
    fun `every curve is ascending from empty to full`() {
        for ((idx, chem) in BatteryModel.CHEMS.withIndex()) {
            if (!BatteryModel.hasCurve(idx)) continue
            for (k in 1 until BatteryModel.OCV_N) {
                assertTrue(
                    "${chem.name}: ocv[$k] must be >= ocv[${k - 1}] for an unambiguous lookup",
                    chem.ocv[k] >= chem.ocv[k - 1],
                )
            }
        }
    }

    @Test
    fun `a full pack of maxCells stays inside the EL15's 60 V rating`() {
        for ((idx, chem) in BatteryModel.CHEMS.withIndex()) {
            if (!BatteryModel.hasCurve(idx)) continue
            val fullPack = chem.maxCells * chem.full
            assertTrue(
                "${chem.name}: ${chem.maxCells}S at ${chem.full} V/cell = $fullPack V exceeds 60 V",
                fullPack <= El15Protocol.MAX_VOLTAGE_V,
            )
        }
    }

    @Test
    fun `curve endpoints map to empty and full`() {
        for ((idx, chem) in BatteryModel.CHEMS.withIndex()) {
            if (!BatteryModel.hasCurve(idx)) continue
            assertEquals("${chem.name} bottom", 0f, BatteryModel.socFromOcv(idx, chem.ocv[0]), 1e-4f)
            assertEquals("${chem.name} top", 1f,
                BatteryModel.socFromOcv(idx, chem.ocv[BatteryModel.OCV_N - 1]), 1e-4f)
        }
    }

    @Test
    fun `out-of-range voltages clamp rather than extrapolate`() {
        val liion = 0
        // Past either end the curve says nothing useful, and a pack reading above
        // "full" is a wrong cell count, not a 130 % charge.
        assertEquals(0f, BatteryModel.socFromOcv(liion, 0.5f), 1e-6f)
        assertEquals(1f, BatteryModel.socFromOcv(liion, 9.9f), 1e-6f)
    }

    @Test
    fun `state of charge rises monotonically with voltage`() {
        val liion = 0
        var previous = -1f
        var v = 2.9f
        while (v <= 4.3f) {
            val soc = BatteryModel.socFromOcv(liion, v)
            assertTrue("SoC must not fall as voltage rises (at $v V)", soc >= previous)
            previous = soc
            v += 0.01f
        }
    }

    @Test
    fun `a mid-curve voltage interpolates sensibly`() {
        val liion = 0
        // 3.73 V is the tabulated midpoint of the Li-ion curve (index 5 of 0..10).
        assertEquals(0.5f, BatteryModel.socFromOcv(liion, 3.73f), 1e-3f)
    }

    @Test
    fun `custom chemistry has no curve and reports no state of charge`() {
        val custom = BatteryModel.CUSTOM
        assertFalse(BatteryModel.hasCurve(custom))
        assertEquals(-1f, BatteryModel.socFromOcv(custom, 3.7f), 1e-6f)
    }

    @Test
    fun `an out-of-range chemistry index is handled`() {
        assertFalse(BatteryModel.hasCurve(-1))
        assertFalse(BatteryModel.hasCurve(BatteryModel.CHEM_N))
        assertEquals(-1f, BatteryModel.socFromOcv(-1, 3.7f), 1e-6f)
        assertEquals(0.2f, BatteryModel.stdCRate(-1), 1e-6f)
    }

    @Test
    fun `standard C-rate is one of the offered rates`() {
        for ((idx, chem) in BatteryModel.CHEMS.withIndex()) {
            assertTrue("${chem.name}: stdRate index out of range",
                chem.stdRate in 0 until BatteryModel.CRATE_N)
            assertEquals(chem.cRate[chem.stdRate], BatteryModel.stdCRate(idx), 1e-6f)
        }
    }

    @Test
    fun `lead-acid is rated at the C20 hour rate and is a fixed six-cell pack`() {
        val pb = BatteryModel.CHEMS.indexOfFirst { it.shortName == "Pb 12V" }
        assertTrue(pb >= 0)
        assertEquals(0.05f, BatteryModel.stdCRate(pb), 1e-6f)
        assertTrue(BatteryModel.cellsFixed(pb))
        assertEquals(6, BatteryModel.CHEMS[pb].fixedCells)
    }

    @Test
    fun `C-rates are offered in ascending order`() {
        for (chem in BatteryModel.CHEMS) {
            for (k in 1 until BatteryModel.CRATE_N) {
                assertTrue("${chem.name}: C-rates must ascend", chem.cRate[k] > chem.cRate[k - 1])
            }
        }
    }

    @Test
    fun `cell-count suggestion lands on the obvious pack`() {
        val liion = 0
        assertEquals(3, BatteryModel.suggestCells(liion, 11.6f))   // a 3S pack, mid-charge
        assertEquals(4, BatteryModel.suggestCells(liion, 15.5f))
        // A fixed-size chemistry ignores the reading and returns its own size.
        val pb = BatteryModel.CHEMS.indexOfFirst { it.shortName == "Pb 12V" }
        assertEquals(6, BatteryModel.suggestCells(pb, 12.7f))
        // Never proposes a pack the load cannot take.
        assertTrue(BatteryModel.suggestCells(liion, 500f) <= BatteryModel.CHEMS[liion].maxCells)
    }
}
