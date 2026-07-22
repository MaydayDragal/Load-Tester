package com.loadtester.el15sim

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Locks in the wire format: the simulator must decode the exact command frames
 * the app/ESP sends and encode status packets the app's parser accepts (header
 * + zero checksum + little-endian float fields at the known offsets).
 */
class LoadModelTest {

    private fun f32(a: ByteArray, off: Int): Float {
        val bits = (a[off].toInt() and 0xFF) or ((a[off + 1].toInt() and 0xFF) shl 8) or
            ((a[off + 2].toInt() and 0xFF) shl 16) or ((a[off + 3].toInt() and 0xFF) shl 24)
        return java.lang.Float.intBitsToFloat(bits)
    }

    @Test
    fun decodesTheFixedCommandFrames() {
        val m = LoadModel()
        assertEquals("POLL", m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x08, 0x00, 0x3F)).description)

        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x09, 0x01, 0x04))
        assertTrue("load on", m.loadOn)
        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x09, 0x01, 0x00))
        assertTrue("load off", !m.loadOn)

        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x03, 0x01, LoadModel.MODE_CV.toByte()))
        assertEquals(LoadModel.MODE_CV, m.mode)
    }

    @Test
    fun decodesSetpointFloat() {
        val m = LoadModel()
        // 2.5 A as little-endian float32 = 00 00 20 40
        val cmd = byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x04, 0x04, 0x00, 0x00, 0x20, 0x40)
        m.decode(cmd)
        assertEquals(2.5f, m.setpoint, 1e-4f)
    }

    @Test
    fun statusPacketHasHeaderAndZeroChecksum() {
        val m = LoadModel()
        val pkt = m.buildStatusPacket(0)
        assertEquals(28, pkt.size)
        // Header DF 07 03 08
        assertEquals(0xDF, pkt[0].toInt() and 0xFF)
        assertEquals(0x07, pkt[1].toInt() and 0xFF)
        assertEquals(0x03, pkt[2].toInt() and 0xFF)
        assertEquals(0x08, pkt[3].toInt() and 0xFF)
        // App's parser checks (sum of all bytes) & 0xFF == 0.
        val sum = pkt.sumOf { it.toInt() and 0xFF } and 0xFF
        assertEquals(0, sum)
    }

    @Test
    fun openCircuitVoltageEncodesEmf() {
        val m = LoadModel()
        m.emf = 12.6f; m.seriesR = 0.35f
        // Load off: terminal voltage equals emf (within the tiny simulated noise).
        val pkt = m.buildStatusPacket(0)
        assertEquals(12.6f, f32(pkt, 7), 0.1f)
    }

    // ---- Battery simulation ------------------------------------------------

    private fun batteryModel(): LoadModel = LoadModel().apply {
        batteryMode = true
        chemistry = LoadModel.CHEM_LIION
        cells = 3
        batteryCapacityAh = 2.0f
        batteryR = 0.1f
        recharge()
    }

    private fun cc(m: LoadModel, amps: Float) {
        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x03, 0x01, LoadModel.MODE_CC.toByte()))
        val bits = java.lang.Float.floatToIntBits(amps)
        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x04, 0x04,
            bits.toByte(), (bits ushr 8).toByte(), (bits ushr 16).toByte(), (bits ushr 24).toByte()))
        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x09, 0x01, 0x04)) // load on
    }

    @Test
    fun ocvCurvesAreMonotonicWithKnee() {
        for (chem in 0..3) {
            var prev = LoadModel.ocvPerCell(chem, 0f)
            var soc = 0.05f
            while (soc <= 1.001f) {
                val v = LoadModel.ocvPerCell(chem, soc)
                assertTrue("chem $chem OCV must not decrease as SoC rises", v >= prev)
                prev = v
                soc += 0.05f
            }
            // The empty-end knee must be steeper than the mid plateau.
            val knee = LoadModel.ocvPerCell(chem, 0.05f) - LoadModel.ocvPerCell(chem, 0f)
            val plateau = LoadModel.ocvPerCell(chem, 0.55f) - LoadModel.ocvPerCell(chem, 0.50f)
            assertTrue("chem $chem knee steeper than plateau", knee > plateau)
        }
    }

    @Test
    fun lifepo4PlateauIsFlat() {
        val v80 = LoadModel.ocvPerCell(LoadModel.CHEM_LIFEPO4, 0.8f)
        val v30 = LoadModel.ocvPerCell(LoadModel.CHEM_LIFEPO4, 0.3f)
        assertTrue("LiFePO4 plateau flat (<0.1 V over 30-80%)", v80 - v30 < 0.10f)
    }

    @Test
    fun fullBatteryVoltageMatchesCurveTimesCells() {
        val m = batteryModel()
        val pkt = m.buildStatusPacket(0)
        // 3S Li-ion at 100%: 3 * 4.20 = 12.6 V open circuit.
        assertEquals(3 * 4.20f, f32(pkt, 7), 0.1f)
    }

    @Test
    fun terminalVoltageSagsByInternalResistance() {
        val m = batteryModel()
        cc(m, 2.0f)
        val pkt = m.buildStatusPacket(100)
        val expected = m.ocvNow() - 2.0f * 0.1f
        assertEquals(expected, f32(pkt, 7), 0.15f)
    }

    @Test
    fun coulombCountingDrainsSocAndFollowsCurve() {
        val m = batteryModel()
        cc(m, 2.0f)
        // Discharge 2 A from a 2 Ah pack for 30 simulated minutes → 1 Ah → 50%.
        repeat(1800) { m.buildStatusPacket(1000) }
        assertEquals(1.0, m.drawnAh, 0.02)
        assertEquals(0.5, m.soc, 0.01)
        // OCV must now sit on the 50% point of the curve.
        assertEquals(3 * LoadModel.ocvPerCell(LoadModel.CHEM_LIION, 0.5f), m.ocvNow(), 0.02f)
        // And the discharge continues into the knee: after ~28 more minutes the
        // pack is nearly empty and the loaded terminal voltage has collapsed
        // toward the 0% end of the curve.
        repeat(1700) { m.buildStatusPacket(1000) }
        assertTrue("SoC nearly empty, was ${m.soc}", m.soc < 0.06)
        val vEnd = f32(m.buildStatusPacket(1000), 7)
        assertTrue("terminal voltage in the knee, was $vEnd", vEnd < 3 * 3.55f)
    }

    @Test
    fun socClampsAtZeroAndRechargeRestores() {
        val m = batteryModel()
        cc(m, 4.0f)
        repeat(4000) { m.buildStatusPacket(1000) }  // way past empty
        assertTrue(m.soc >= 0.0)
        assertEquals(0.0, m.soc, 0.001)
        m.recharge()
        assertEquals(1.0, m.soc, 1e-6)
        assertEquals(0.0, m.drawnAh, 1e-6)
        m.recharge(40f)
        assertEquals(0.4, m.soc, 1e-6)
    }

    @Test
    fun leadAcidCapacityTestReachesConventionalCutoff() {
        // The app's lead-acid preset cuts off at 1.75 V/cell (10.5 V for 6S).
        // The simulated pack must actually cross that at a gentle 0.2C rate,
        // or a capacity test against the simulator would never terminate.
        val m = LoadModel().apply {
            batteryMode = true
            chemistry = LoadModel.CHEM_LEAD
            cells = 6
            batteryCapacityAh = 2.0f
            batteryR = 0.05f
            recharge()
        }
        cc(m, 0.4f)  // 0.2C
        val cutoff = 6 * 1.75f
        var reached = false
        // Up to 8 simulated hours in 1 s ticks.
        for (t in 0 until 8 * 3600) {
            val v = f32(m.buildStatusPacket(1000), 7)
            if (v <= cutoff) { reached = true; break }
        }
        assertTrue("terminal voltage must reach the 1.75 V/cell cutoff", reached)
    }

    @Test
    fun overdrawnPackVoltageCollapses() {
        // Even with a cutoff set below the chemistry curve's 0 % floor, a dead
        // pack must not source charge forever — voltage collapses on overdraw.
        val m = batteryModel()
        cc(m, 4.0f)
        repeat(2 * 3600) { m.buildStatusPacket(1000) }  // 8 Ah from a 2 Ah pack
        assertEquals(0.0, m.soc, 1e-6)
        val v = f32(m.buildStatusPacket(1000), 7)
        assertTrue("dead pack voltage collapsed, was $v", v < 1.0f)
        // Recharge fully restores it.
        m.recharge()
        assertEquals(3 * 4.20f, m.ocvNow(), 0.05f)
    }

    @Test
    fun nonFiniteSetpointIsRejected() {
        val m = LoadModel()
        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x03, 0x01, LoadModel.MODE_CC.toByte()))
        // NaN as little-endian float32.
        val bits = java.lang.Float.floatToIntBits(Float.NaN)
        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x04, 0x04,
            bits.toByte(), (bits ushr 8).toByte(), (bits ushr 16).toByte(), (bits ushr 24).toByte()))
        assertEquals(0f, m.setpoint, 1e-6f)
        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x09, 0x01, 0x04)) // load on
        val pkt = m.buildStatusPacket(500)
        // Every encoded field must stay finite.
        assertTrue(f32(pkt, 7).isFinite())
        assertTrue(f32(pkt, 11).isFinite())
    }

    @Test
    fun unknownModeBytesAreIgnored() {
        val m = LoadModel()
        val before = m.mode
        // 0x06 would set the warn-flag bits in the status byte if accepted.
        val d = m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x03, 0x01, 0x06))
        assertTrue(!d.known)
        assertEquals(before, m.mode)
        // Packet must not carry a forged warning.
        val pkt = m.buildStatusPacket(0)
        val b5 = pkt[5].toInt() and 0xFF
        assertTrue("warn flag must not be set", (b5 and 0x06) != 0x06)
    }

    @Test
    fun deadPackCollapsesBelowTheOldFloor() {
        // With the battery-mode 0.05 V floor, a fully collapsed pack's terminal
        // voltage falls below 0.1 V, so even absurdly low cutoffs terminate.
        val m = batteryModel()
        cc(m, 4.0f)
        repeat(4 * 3600) { m.buildStatusPacket(1000) }
        val v = f32(m.buildStatusPacket(1000), 7)
        assertTrue("collapsed terminal voltage, was $v", v < 0.1f)
    }

    @Test
    fun batteryPacketsStillChecksumValid() {
        val m = batteryModel()
        cc(m, 1.0f)
        repeat(10) {
            val pkt = m.buildStatusPacket(500)
            assertEquals(28, pkt.size)
            assertEquals(0, pkt.sumOf { it.toInt() and 0xFF } and 0xFF)
        }
    }

    @Test
    fun fixedModeDoesNotDrainBattery() {
        val m = batteryModel()
        m.batteryMode = false
        m.emf = 12.6f; m.seriesR = 0.35f
        cc(m, 2.0f)
        repeat(600) { m.buildStatusPacket(1000) }
        assertEquals(1.0, m.soc, 1e-6)
        assertEquals(0.0, m.drawnAh, 1e-6)
    }

    @Test
    fun loadedVoltageSagsByIrDrop() {
        val m = LoadModel()
        m.emf = 12.6f; m.seriesR = 0.5f
        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x03, 0x01, LoadModel.MODE_CC.toByte()))
        // 2 A setpoint → expect ~12.6 - 2*0.5 = 11.6 V, 2 A.
        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x04, 0x04, 0x00, 0x00, 0x00, 0x40)) // 2.0f
        m.decode(byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x09, 0x01, 0x04)) // load on
        val pkt = m.buildStatusPacket(100)
        val v = f32(pkt, 7)
        val i = f32(pkt, 11)
        assertEquals(2.0f, i, 0.05f)
        assertEquals(11.6f, v, 0.2f)
    }
}
