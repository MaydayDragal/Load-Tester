package com.loadtester.el15

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** JSON round-trip tests for the archived-test model. */
class TestRecordTest {

    private fun sample() = TestRecord(
        id = "t1700000000000",
        timestampMs = 1_700_000_000_000L,
        deviceLabel = "Demo (12.6 V, 0.350 Ω)",
        fuseRating = 10f,
        safetyPct = 80,
        steps = 8,
        settleMs = 800L,
        sampleMs = 1500L,
        maxTestCurrent = 8f,
        resistanceOhm = 0.349f,
        openCircuitVoltage = 12.596f,
        rSquared = 0.9999f,
        reliable = true,
        samples = listOf(
            CircuitResistanceTester.Sample(1f, 12.25f, 25.4f, 0),
            CircuitResistanceTester.Sample(2f, 11.90f, 26.3f, 1),
            CircuitResistanceTester.Sample(8f, 9.80f, 31.7f, 3),
        ),
        notes = "Bench test\nline two",
        resistanceStdErr = 0.0041f,
    )

    @Test fun jsonRoundTripPreservesEverything() {
        val original = sample()
        val restored = TestRecord.fromJson(JSONObject(original.toJson().toString()))
        assertEquals(original.id, restored.id)
        assertEquals(original.timestampMs, restored.timestampMs)
        assertEquals(original.deviceLabel, restored.deviceLabel)
        assertEquals(original.fuseRating, restored.fuseRating, 1e-4f)
        assertEquals(original.safetyPct, restored.safetyPct)
        assertEquals(original.steps, restored.steps)
        assertEquals(original.settleMs, restored.settleMs)
        assertEquals(original.sampleMs, restored.sampleMs)
        assertEquals(original.maxTestCurrent, restored.maxTestCurrent, 1e-4f)
        assertEquals(original.resistanceOhm, restored.resistanceOhm, 1e-5f)
        assertEquals(original.openCircuitVoltage, restored.openCircuitVoltage, 1e-4f)
        assertEquals(original.rSquared, restored.rSquared, 1e-5f)
        assertEquals(original.resistanceStdErr, restored.resistanceStdErr, 1e-6f)
        assertEquals(original.reliable, restored.reliable)
        assertEquals(original.notes, restored.notes)
        assertEquals(original.samples.size, restored.samples.size)
        original.samples.zip(restored.samples).forEach { (a, b) ->
            assertEquals(a.current, b.current, 1e-4f)
            assertEquals(a.voltage, b.voltage, 1e-4f)
            assertEquals(a.temperature, b.temperature, 1e-4f)
            assertEquals(a.fanSpeed, b.fanSpeed)
        }
    }

    @Test fun derivedMetricsComputeFromSamples() {
        val r = sample()
        assertEquals(8f * 9.80f, r.peakPower, 1e-2f)
        assertEquals(31.7f, r.maxTemp, 1e-3f)
        assertEquals(3, r.maxFan)
    }

    @Test fun missingOptionalFieldsFallBack() {
        // Simulate an older/foreign record with only required fields.
        val o = sample().toJson()
        o.remove("deviceLabel"); o.remove("safetyPct"); o.remove("notes"); o.remove("reliable")
        val restored = TestRecord.fromJson(o)
        assertEquals("EL15", restored.deviceLabel)
        assertEquals(80, restored.safetyPct)
        assertEquals("", restored.notes)
        assertTrue(!restored.reliable)
    }

    @Test fun chartConfigEncodeDecodeRoundTrip() {
        val c = TestChartView.ChartConfig(
            view = TestChartView.ChartConfig.VIEW_R_I,
            grid = false, markers = true, fill = true, thick = false,
        )
        c.series.clear(); c.series.addAll(listOf('V', 'P'))
        val d = TestChartView.ChartConfig.decode(c.encode())
        assertEquals(c.view, d.view)
        assertEquals(c.series, d.series)
        assertEquals(c.grid, d.grid)
        assertEquals(c.markers, d.markers)
        assertEquals(c.fill, d.fill)
        assertEquals(c.thick, d.thick)
    }

    @Test fun chartConfigDecodeGarbageFallsBack() {
        val d = TestChartView.ChartConfig.decode("not|a|config")
        assertEquals(TestChartView.ChartConfig.VIEW_VI, d.view)
        val e = TestChartView.ChartConfig.decode(null)
        assertTrue(e.grid)
    }
}
