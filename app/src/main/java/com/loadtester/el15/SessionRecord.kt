package com.loadtester.el15

import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale

/**
 * A completed long-running bench test (battery capacity, constant-power
 * runtime, step-load transient, or OCP ramp): parameters, a time series of
 * readings, and computed metrics. Archived alongside [TestRecord]s.
 */
data class SessionRecord(
    val id: String,
    val timestampMs: Long,
    val type: Int,
    val deviceLabel: String,
    var tag: String = "",
    var notes: String = "",
    /** Test parameters, e.g. dischargeA, cutoffV, ratedAh, powerW, iLow, iHigh… */
    val params: MutableMap<String, Float> = LinkedHashMap(),
    /** Computed results, e.g. capacityAh, energyWh, runtimeS, sohPct, tripA… */
    val metrics: MutableMap<String, Float> = LinkedHashMap(),
    /** (elapsed ms, V, I, temp, fan) — decimated to a bounded size. */
    val points: MutableList<TimePoint> = ArrayList(),
) {
    data class TimePoint(val tMs: Long, val v: Float, val i: Float, val temp: Float, val fan: Int) {
        val p: Float get() = v * i
    }

    fun typeName(): String = when (type) {
        TYPE_CAPACITY -> "Battery capacity test"
        TYPE_RUNTIME -> "Runtime test"
        TYPE_STEP -> "Step-load test"
        TYPE_OCP -> "OCP ramp"
        else -> "Session"
    }

    /** True when the test was stopped/interrupted before its natural end. */
    val isPartial: Boolean get() = (metrics["aborted"] ?: 0f) > 0f

    fun headline(): String {
        val prefix = if (isPartial) "Partial · " else ""
        return prefix + when (type) {
            TYPE_CAPACITY -> {
                val ah = metrics["capacityAh"] ?: 0f
                val soh = metrics["sohPct"]
                if (soh != null && soh > 0f) "%.2f Ah · SoH %.0f%%".format(ah, soh)
                else "%.2f Ah".format(ah)
            }
            TYPE_RUNTIME -> "Runtime %s".format(fmtDuration(metrics["runtimeS"]?.toLong() ?: 0L))
            TYPE_STEP -> "Droop %.0f mV · rec %.0f ms".format(
                (metrics["droopV"] ?: 0f) * 1000f, metrics["recoveryMs"] ?: 0f)
            TYPE_OCP -> if ((metrics["tripA"] ?: 0f) > 0f) "Trip at %.2f A".format(metrics["tripA"])
            else "No trip up to %.2f A".format(metrics["maxA"] ?: 0f)
            else -> ""
        }
    }

    fun toJson(): JSONObject = JSONObject().apply {
        put("kind", "session"); put("version", 1)
        put("id", id); put("timestampMs", timestampMs); put("type", type)
        put("deviceLabel", deviceLabel); put("tag", tag); put("notes", notes)
        put("params", JSONObject(params.mapValues { it.value.toDouble() }))
        put("metrics", JSONObject(metrics.mapValues { it.value.toDouble() }))
        put("points", JSONArray().apply {
            for (p in points) put(JSONArray().apply {
                put(p.tMs); put(p.v.toDouble()); put(p.i.toDouble())
                put(p.temp.toDouble()); put(p.fan)
            })
        })
    }

    companion object {
        const val TYPE_CAPACITY = 1
        const val TYPE_RUNTIME = 2
        const val TYPE_STEP = 3
        const val TYPE_OCP = 4

        fun fmtDuration(s: Long): String = when {
            s >= 3600 -> "%dh %02dm".format(s / 3600, (s % 3600) / 60)
            s >= 60 -> "%dm %02ds".format(s / 60, s % 60)
            else -> "${s}s"
        }

        fun fromJson(o: JSONObject): SessionRecord {
            val rec = SessionRecord(
                id = o.getString("id"),
                timestampMs = o.getLong("timestampMs"),
                type = o.getInt("type"),
                deviceLabel = o.optString("deviceLabel", "EL15"),
                tag = o.optString("tag", ""),
                notes = o.optString("notes", ""),
            )
            o.optJSONObject("params")?.let { po ->
                po.keys().forEach { k -> rec.params[k] = po.getDouble(k).toFloat() }
            }
            o.optJSONObject("metrics")?.let { mo ->
                mo.keys().forEach { k -> rec.metrics[k] = mo.getDouble(k).toFloat() }
            }
            val arr = o.optJSONArray("points") ?: JSONArray()
            for (k in 0 until arr.length()) {
                val row = arr.getJSONArray(k)
                rec.points.add(TimePoint(row.getLong(0), row.getDouble(1).toFloat(),
                    row.getDouble(2).toFloat(), row.getDouble(3).toFloat(), row.getInt(4)))
            }
            return rec
        }
    }

    fun toCsv(): String {
        val sb = StringBuilder()
        sb.append("# ${typeName()}\n")
        params.forEach { (k, v) -> sb.append("# param_%s,%.4f\n".format(Locale.US, k, v)) }
        metrics.forEach { (k, v) -> sb.append("# metric_%s,%.4f\n".format(Locale.US, k, v)) }
        if (notes.isNotBlank()) sb.append("# notes,${notes.replace("\n", " ")}\n")
        sb.append("elapsed_ms,voltage_V,current_A,power_W,temp_C,fan\n")
        for (p in points) sb.append("%d,%.4f,%.4f,%.4f,%.2f,%d\n"
            .format(Locale.US, p.tMs, p.v, p.i, p.p, p.temp, p.fan))
        return sb.toString()
    }
}
