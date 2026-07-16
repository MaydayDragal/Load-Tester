package com.loadtester.el15

import org.json.JSONArray
import org.json.JSONObject

/**
 * A completed circuit-resistance test, persisted in the app's test archive and
 * recallable from the History screen. JSON-serialized (org.json, no deps).
 */
data class TestRecord(
    val id: String,
    val timestampMs: Long,
    val deviceLabel: String,
    val fuseRating: Float,
    val safetyPct: Int,
    val steps: Int,
    val settleMs: Long,
    val sampleMs: Long,
    val maxTestCurrent: Float,
    val resistanceOhm: Float,
    val openCircuitVoltage: Float,
    val rSquared: Float,
    val reliable: Boolean,
    val samples: List<CircuitResistanceTester.Sample>,
    var notes: String = "",
) {
    val peakPower: Float get() = samples.maxOfOrNull { it.power } ?: 0f
    val maxTemp: Float get() = samples.maxOfOrNull { it.temperature } ?: 0f
    val maxFan: Int get() = samples.maxOfOrNull { it.fanSpeed } ?: 0

    fun toJson(): JSONObject = JSONObject().apply {
        put("version", 1)
        put("id", id)
        put("timestampMs", timestampMs)
        put("deviceLabel", deviceLabel)
        put("fuseRating", fuseRating.toDouble())
        put("safetyPct", safetyPct)
        put("steps", steps)
        put("settleMs", settleMs)
        put("sampleMs", sampleMs)
        put("maxTestCurrent", maxTestCurrent.toDouble())
        put("resistanceOhm", resistanceOhm.toDouble())
        put("openCircuitVoltage", openCircuitVoltage.toDouble())
        put("rSquared", rSquared.toDouble())
        put("reliable", reliable)
        put("notes", notes)
        put("samples", JSONArray().apply {
            for (s in samples) put(JSONArray().apply {
                put(s.current.toDouble()); put(s.voltage.toDouble())
                put(s.temperature.toDouble()); put(s.fanSpeed)
            })
        })
    }

    companion object {
        fun fromJson(o: JSONObject): TestRecord {
            val arr = o.getJSONArray("samples")
            val samples = ArrayList<CircuitResistanceTester.Sample>(arr.length())
            for (k in 0 until arr.length()) {
                val row = arr.getJSONArray(k)
                samples.add(
                    CircuitResistanceTester.Sample(
                        row.getDouble(0).toFloat(),
                        row.getDouble(1).toFloat(),
                        row.getDouble(2).toFloat(),
                        row.getInt(3),
                    )
                )
            }
            return TestRecord(
                id = o.getString("id"),
                timestampMs = o.getLong("timestampMs"),
                deviceLabel = o.optString("deviceLabel", "EL15"),
                fuseRating = o.getDouble("fuseRating").toFloat(),
                safetyPct = o.optInt("safetyPct", 80),
                steps = o.optInt("steps", samples.size),
                settleMs = o.optLong("settleMs", 0),
                sampleMs = o.optLong("sampleMs", 0),
                maxTestCurrent = o.getDouble("maxTestCurrent").toFloat(),
                resistanceOhm = o.getDouble("resistanceOhm").toFloat(),
                openCircuitVoltage = o.getDouble("openCircuitVoltage").toFloat(),
                rSquared = o.getDouble("rSquared").toFloat(),
                reliable = o.optBoolean("reliable", false),
                samples = samples,
                notes = o.optString("notes", ""),
            )
        }

        fun from(
            result: CircuitResistanceTester.ResistanceResult,
            steps: Int, settleMs: Long, sampleMs: Long, safetyPct: Int,
            deviceLabel: String, timestampMs: Long,
        ): TestRecord = TestRecord(
            id = "t$timestampMs",
            timestampMs = timestampMs,
            deviceLabel = deviceLabel,
            fuseRating = result.fuseRating,
            safetyPct = safetyPct,
            steps = steps,
            settleMs = settleMs,
            sampleMs = sampleMs,
            maxTestCurrent = result.maxTestCurrent,
            resistanceOhm = result.resistanceOhm,
            openCircuitVoltage = result.openCircuitVoltage,
            rSquared = result.rSquared,
            reliable = result.reliable,
            samples = result.samples,
        )
    }
}
