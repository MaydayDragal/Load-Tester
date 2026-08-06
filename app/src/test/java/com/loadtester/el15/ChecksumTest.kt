package com.loadtester.el15

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The trailing sum-to-zero checksum on command frames.
 *
 * This is the defect that made the previous version of this app unable to drive
 * real hardware: its command constants were hand-written as prefix+value with no
 * checksum byte, so POLL worked (its 0x3F was captured whole) while mode,
 * setpoint and load commands were silently dropped by a real EL15. Found and
 * fixed on the ESP firmware 2026-07-24 by bench testing; these tests pin the
 * ported fix against the exact frames that firmware sends.
 */
class ChecksumTest {

    private fun sum(frame: ByteArray): Int = frame.sumOf { it.toInt() and 0xFF } and 0xFF

    @Test
    fun `every fixed frame sums to zero`() {
        for ((name, frame) in listOf(
            "POLL" to El15Protocol.POLL,
            "LOAD_ON" to El15Protocol.LOAD_ON,
            "LOAD_OFF" to El15Protocol.LOAD_OFF,
            "LOCK" to El15Protocol.LOCK,
        )) {
            assertEquals("$name must sum to 0 mod 256", 0, sum(frame))
        }
    }

    /**
     * Byte-exact against the firmware's `el15_protocol.h` constants — the ones a
     * real EL15 has been verified to honour.
     */
    @Test
    fun `fixed frames match the hardware-verified bytes`() {
        assertArrayEquals(
            byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x08, 0x00, 0x3F), El15Protocol.POLL)
        assertArrayEquals(
            byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x09, 0x01, 0x04, 0x39), El15Protocol.LOAD_ON)
        assertArrayEquals(
            byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x09, 0x01, 0x00, 0x3D), El15Protocol.LOAD_OFF)
        assertArrayEquals(
            byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x09, 0x01, 0x01, 0x3C), El15Protocol.LOCK)
    }

    @Test
    fun `mode commands carry a checksum`() {
        for (mode in El15Protocol.SELECTABLE_MODES) {
            val frame = El15Protocol.modeCommand(mode)
            assertEquals("mode $mode frame length", 7, frame.size)
            assertEquals("mode $mode must sum to 0", 0, sum(frame))
            assertEquals("mode byte", mode.toByte(), frame[5])
        }
    }

    @Test
    fun `setpoint commands carry a checksum and a little-endian float`() {
        for (v in listOf(0f, 0.05f, 1.234f, 12f, 59.999f)) {
            val frame = El15Protocol.setpointCommand(v)
            assertEquals("setpoint frame length", 10, frame.size)
            assertEquals("setpoint $v must sum to 0", 0, sum(frame))
            val bits = (frame[5].toInt() and 0xFF) or
                ((frame[6].toInt() and 0xFF) shl 8) or
                ((frame[7].toInt() and 0xFF) shl 16) or
                ((frame[8].toInt() and 0xFF) shl 24)
            assertEquals("payload round-trips", v, Float.fromBits(bits), 0f)
        }
    }

    @Test
    fun `frameChecksum is the two's complement of the byte sum`() {
        assertEquals(0x3F.toByte(), El15Protocol.frameChecksum(
            byteArrayOf(0xAF.toByte(), 0x07, 0x03, 0x08, 0x00)))
        // A frame that already sums to zero needs a zero byte, not 0x100.
        assertEquals(0.toByte(), El15Protocol.frameChecksum(byteArrayOf(0x00)))
        assertEquals(0.toByte(), El15Protocol.frameChecksum(byteArrayOf(0x80.toByte(), 0x80.toByte())))
    }

    /**
     * The parser enforces the same rule on incoming packets, so a frame built
     * here and fed back in must pass its check. Guards against the checksum
     * convention drifting on one side only.
     */
    @Test
    fun `built frames satisfy the same rule the parser enforces`() {
        val frames = listOf(
            El15Protocol.POLL, El15Protocol.LOAD_ON, El15Protocol.LOAD_OFF, El15Protocol.LOCK,
            El15Protocol.modeCommand(El15Protocol.MODE_CC),
            El15Protocol.setpointCommand(2.5f),
        )
        for (f in frames) assertEquals(0, sum(f))
    }
}
