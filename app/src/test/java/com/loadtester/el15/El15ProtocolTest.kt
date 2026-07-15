package com.loadtester.el15

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Pure-JVM tests for the EL15 BLE protocol layer — command frames and the
 * 28-byte status-packet parser. These byte layouts are safety-relevant (the
 * resistance sweep sizes its current ladder from parsed voltage), so they are
 * pinned here against the DM40GUI reference implementation.
 */
class El15ProtocolTest {

    // ---- Command frames ----------------------------------------------------

    @Test fun pollFrameMatchesReference() {
        assertEquals("AF 07 03 08 00 3F", El15Protocol.POLL.hex())
    }

    @Test fun loadOnOffLockFramesMatchReference() {
        assertEquals("AF 07 03 09 01 04", El15Protocol.LOAD_ON.hex())
        assertEquals("AF 07 03 09 01 00", El15Protocol.LOAD_OFF.hex())
        assertEquals("AF 07 03 09 01 01", El15Protocol.LOCK.hex())
    }

    @Test fun modeCommandAppendsModeByte() {
        assertEquals("AF 07 03 03 01 01", El15Protocol.modeCommand(El15Protocol.MODE_CC).hex())
        assertEquals("AF 07 03 03 01 19", El15Protocol.modeCommand(El15Protocol.MODE_CP).hex())
        assertEquals("AF 07 03 03 01 0A", El15Protocol.modeCommand(El15Protocol.MODE_DCR).hex())
    }

    @Test fun setpointCommandEncodesLittleEndianFloat() {
        val cmd = El15Protocol.setpointCommand(1.5f)
        assertEquals(9, cmd.size)
        assertEquals("AF 07 03 04 04", cmd.copyOfRange(0, 5).hex())
        val f = ByteBuffer.wrap(cmd, 5, 4).order(ByteOrder.LITTLE_ENDIAN).float
        assertEquals(1.5f, f, 0f)
    }

    // ---- Status packet parsing --------------------------------------------

    private fun buildPacket(
        mode: Int,
        voltage: Float,
        current: Float,
        loadOn: Boolean = true,
        lockOn: Boolean = false,
        fan: Int = 0,
        runtime: Int = 0,
        slot2: Float = 0f, // temp | energy(mWh) | I2
        slot3: Float = 0f, // setpoint | capacity(mAh) | mOhm
        warn: Boolean = false,
        warnCode: Int = 0,
        dcrI1: Float = 0f,
    ): ByteArray {
        val p = ByteArray(28)
        p[0] = 0xDF.toByte(); p[1] = 0x07; p[2] = 0x03; p[3] = 0x08
        var b5 = (mode and 0x1F) or ((fan and 0x03) shl 6)
        if (warn) b5 = b5 or 0x06
        p[5] = b5.toByte()
        var b6 = (if (loadOn) 0x02 else 0) or (if (lockOn) 0x04 else 0) or ((fan shr 2) and 0x01)
        if (warn) b6 = b6 or (warnCode shl 4)
        p[6] = b6.toByte()
        putF(p, 7, voltage); putF(p, 11, current)
        if (mode == El15Protocol.MODE_DCR) putF(p, 15, dcrI1) else putI(p, 15, runtime)
        putF(p, 19, slot2); putF(p, 23, slot3)
        var sum = 0
        for (k in 0..26) sum += p[k].toInt() and 0xFF
        p[27] = ((-sum) and 0xFF).toByte()
        return p
    }

    @Test fun parsesCcPacket() {
        val s = El15Protocol.parseStatus(
            buildPacket(El15Protocol.MODE_CC, 11.90f, 2.0f, runtime = 42, slot2 = 26.5f, slot3 = 2.0f, fan = 3)
        )
        assertTrue(s.valid); assertTrue(s.crcPass)
        assertEquals("CC", s.modeName)
        assertEquals(11.90f, s.voltage, 1e-4f)
        assertEquals(2.0f, s.current, 1e-4f)
        assertEquals(11.90f * 2.0f, s.power, 1e-3f)
        assertEquals(42, s.runtime)
        assertEquals(26.5f, s.temperature, 1e-4f)
        assertEquals(2.0f, s.setpoint, 1e-4f)
        assertEquals(3, s.fanSpeed)
        assertTrue(s.loadOn); assertFalse(s.lockOn)
        assertTrue(s.setpointInPacket)
        assertEquals("", s.warning)
    }

    @Test fun parsesCapPacketWithMilliScaling() {
        val s = El15Protocol.parseStatus(
            buildPacket(El15Protocol.MODE_CAP, 12.0f, 3.0f, runtime = 55, slot2 = 150f, slot3 = 1200f)
        )
        assertEquals("CAP", s.modeName)
        assertEquals(0.150f, s.energyWh, 1e-5f)
        assertEquals(1.2f, s.capacityAh, 1e-5f)
        assertFalse(s.setpointInPacket)
    }

    @Test fun parsesDcrPacketAndZeroesLoadFigures() {
        val s = El15Protocol.parseStatus(
            buildPacket(El15Protocol.MODE_DCR, 12.4f, 1.5f, dcrI1 = 1.5f, slot2 = 0.75f, slot3 = 350f)
        )
        assertEquals("DCR", s.modeName)
        assertEquals(1.5f, s.dcrI1, 1e-4f)
        assertEquals(0.75f, s.dcrI2, 1e-4f)
        assertEquals(350f, s.dcrMilliOhm, 1e-2f)
        assertEquals(0f, s.current, 0f)
        assertEquals(0f, s.power, 0f)
        assertFalse(s.setpointInPacket)
    }

    @Test fun fanSpeedSplitsAcrossBytes() {
        // fan=5 -> b5 bits6-7 = 01, b6 bit0 = 1
        val s = El15Protocol.parseStatus(buildPacket(El15Protocol.MODE_CC, 12f, 1f, fan = 5))
        assertEquals(5, s.fanSpeed)
    }

    @Test fun lockBitParses() {
        val s = El15Protocol.parseStatus(buildPacket(El15Protocol.MODE_CC, 12f, 1f, lockOn = true))
        assertTrue(s.lockOn)
    }

    @Test fun warningPacketDecodesProtectionCode() {
        val rev = El15Protocol.parseStatus(
            buildPacket(El15Protocol.MODE_CC, 12f, 0f, warn = true, warnCode = 0x6)
        )
        assertEquals("REV", rev.warning)
        assertFalse(rev.ready)

        val uvp = El15Protocol.parseStatus(
            buildPacket(El15Protocol.MODE_CC, 12f, 0f, warn = true, warnCode = 0x9)
        )
        assertEquals("UVP", uvp.warning)
    }

    @Test fun badCrcIsFlagged() {
        val p = buildPacket(El15Protocol.MODE_CC, 12f, 1f)
        p[27] = (p[27] + 1).toByte()
        val s = El15Protocol.parseStatus(p)
        assertFalse(s.crcPass)
    }

    @Test fun shortOrForeignPacketIsInvalid() {
        assertFalse(El15Protocol.parseStatus(ByteArray(10)).valid)
        val foreign = buildPacket(El15Protocol.MODE_CC, 12f, 1f).also { it[0] = 0x00 }
        assertFalse(El15Protocol.parseStatus(foreign).valid)
        assertFalse(El15Protocol.isStatusPacket(foreign))
    }

    @Test fun ratingsConstantsMatchHardware() {
        assertEquals(12f, El15Protocol.MAX_CURRENT_A, 0f)
        assertEquals(150f, El15Protocol.MAX_POWER_W, 0f)
        assertEquals(60f, El15Protocol.MAX_VOLTAGE_V, 0f)
    }

    // ---- helpers -----------------------------------------------------------

    private fun ByteArray.hex() = joinToString(" ") { "%02X".format(it) }

    private fun putF(a: ByteArray, off: Int, v: Float) {
        ByteBuffer.wrap(a, off, 4).order(ByteOrder.LITTLE_ENDIAN).putFloat(v)
    }

    private fun putI(a: ByteArray, off: Int, v: Int) {
        ByteBuffer.wrap(a, off, 4).order(ByteOrder.LITTLE_ENDIAN).putInt(v)
    }
}
