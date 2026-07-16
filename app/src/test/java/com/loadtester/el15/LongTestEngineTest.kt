package com.loadtester.el15

import org.junit.Assert.assertEquals
import org.junit.Test

class LongTestEngineTest {

    @Test
    fun estimateCellsMatchesCommonPacks() {
        val liion = LongTestEngine.CHEMISTRIES[0] // 3.7 V nominal
        assertEquals(1, LongTestEngine.estimateCells(3.7f, liion))
        assertEquals(3, LongTestEngine.estimateCells(11.1f, liion))
        assertEquals(4, LongTestEngine.estimateCells(14.8f, liion))
        // Fully charged 3S (12.6 V) still reads as 3 cells, not 4.
        assertEquals(3, LongTestEngine.estimateCells(12.6f, liion))

        val lead = LongTestEngine.CHEMISTRIES[2] // 2.0 V nominal
        assertEquals(6, LongTestEngine.estimateCells(12.6f, lead))

        val lifepo = LongTestEngine.CHEMISTRIES[1] // 3.2 V nominal
        assertEquals(4, LongTestEngine.estimateCells(13.2f, lifepo))
    }

    @Test
    fun estimateCellsClampsToSaneRange() {
        val liion = LongTestEngine.CHEMISTRIES[0]
        assertEquals(1, LongTestEngine.estimateCells(0.1f, liion))
        assertEquals(30, LongTestEngine.estimateCells(500f, liion))
    }

    @Test
    fun chemistriesHaveSaneCutoffs() {
        for (c in LongTestEngine.CHEMISTRIES) {
            // A cutoff above nominal would end every test instantly.
            assert(c.cutoffPerCell < c.nominalPerCell) { c.name }
            assert(c.cutoffPerCell > 0.5f) { c.name }
        }
    }
}
