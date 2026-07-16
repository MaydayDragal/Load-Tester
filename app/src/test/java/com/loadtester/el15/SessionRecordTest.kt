package com.loadtester.el15

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionRecordTest {

    private fun sample(): SessionRecord {
        val rec = SessionRecord(
            id = "s123", timestampMs = 1_700_000_000_000L,
            type = SessionRecord.TYPE_CAPACITY, deviceLabel = "Demo",
            tag = "pack-A", notes = "first run",
        )
        rec.params["dischargeA"] = 1.5f
        rec.params["cutoffV"] = 9.0f
        rec.params["ratedAh"] = 2.5f
        rec.metrics["capacityAh"] = 2.25f
        rec.metrics["sohPct"] = 90f
        rec.points.add(SessionRecord.TimePoint(0L, 12.6f, 1.5f, 25f, 1))
        rec.points.add(SessionRecord.TimePoint(1000L, 12.4f, 1.5f, 26f, 1))
        return rec
    }

    @Test
    fun jsonRoundTripPreservesEverything() {
        val rec = sample()
        val back = SessionRecord.fromJson(JSONObject(rec.toJson().toString()))
        assertEquals(rec.id, back.id)
        assertEquals(rec.timestampMs, back.timestampMs)
        assertEquals(rec.type, back.type)
        assertEquals(rec.tag, back.tag)
        assertEquals(rec.notes, back.notes)
        assertEquals(rec.params.size, back.params.size)
        assertEquals(1.5f, back.params["dischargeA"]!!, 1e-5f)
        assertEquals(2.25f, back.metrics["capacityAh"]!!, 1e-5f)
        assertEquals(rec.points.size, back.points.size)
        assertEquals(rec.points[1].v, back.points[1].v, 1e-5f)
        assertEquals(rec.points[1].fan, back.points[1].fan)
    }

    @Test
    fun headlineShowsCapacityAndSoh() {
        val rec = sample()
        val h = rec.headline()
        assertTrue(h, h.contains("2.25 Ah"))
        assertTrue(h, h.contains("90%"))
    }

    @Test
    fun csvUsesDotDecimalAndHasAllRows() {
        val rec = sample()
        val csv = rec.toCsv()
        assertTrue(csv.contains("elapsed_ms,voltage_V,current_A,power_W,temp_C,fan"))
        // Locale-safe: never comma decimals in data rows.
        val dataRows = csv.lines().filter { it.isNotBlank() && !it.startsWith("#") }
        assertEquals(3, dataRows.size) // header + 2 points
        assertTrue(dataRows[1].startsWith("0,12.6"))
    }

    @Test
    fun fmtDurationScales() {
        assertEquals("45s", SessionRecord.fmtDuration(45))
        assertEquals("2m 05s", SessionRecord.fmtDuration(125))
        assertEquals("3h 05m", SessionRecord.fmtDuration(3 * 3600 + 5 * 60))
    }

    @Test
    fun timePointPowerIsDerived() {
        val p = SessionRecord.TimePoint(0L, 10f, 2f, 25f, 0)
        assertEquals(20f, p.p, 1e-5f)
    }
}
