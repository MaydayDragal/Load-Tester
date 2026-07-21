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
