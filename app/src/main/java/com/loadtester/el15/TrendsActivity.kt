package com.loadtester.el15

import android.os.Bundle
import androidx.lifecycle.lifecycleScope
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.chip.Chip
import com.google.android.material.chip.ChipGroup
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Trends across the archive: pick a metric and see how it moved across every
 * archived test that carries it (resistance drift over time, capacity fade,
 * SoH decline, trip-point movement…). Chart + a per-test table.
 */
class TrendsActivity : BaseActivity() {

    private data class Metric(
        val label: String,
        val unit: String,
        val extract: (tests: List<TestRecord>, sessions: List<SessionRecord>) -> List<TrendChartView.Point>,
    )

    private val metrics = listOf(
        Metric("R (mΩ)", " mΩ") { tests, _ ->
            tests.filter { it.resistanceOhm > 0f }
                .map { TrendChartView.Point(it.timestampMs, it.resistanceOhm * 1000f) }
        },
        Metric("Voc (V)", " V") { tests, _ ->
            tests.filter { it.openCircuitVoltage > 0f }
                .map { TrendChartView.Point(it.timestampMs, it.openCircuitVoltage) }
        },
        Metric("Capacity (Ah)", " Ah") { _, sessions ->
            sessions.filter { it.type == SessionRecord.TYPE_CAPACITY }
                .mapNotNull { s -> s.metrics["capacityAh"]?.let { TrendChartView.Point(s.timestampMs, it) } }
        },
        Metric("SoH (%)", " %") { _, sessions ->
            sessions.filter { it.type == SessionRecord.TYPE_CAPACITY }
                .mapNotNull { s -> s.metrics["sohPct"]?.takeIf { it > 0f }
                    ?.let { TrendChartView.Point(s.timestampMs, it) } }
        },
        Metric("Energy (Wh)", " Wh") { _, sessions ->
            sessions.filter { it.type == SessionRecord.TYPE_CAPACITY || it.type == SessionRecord.TYPE_RUNTIME }
                .mapNotNull { s -> s.metrics["energyWh"]?.let { TrendChartView.Point(s.timestampMs, it) } }
        },
        Metric("Runtime (min)", " min") { _, sessions ->
            sessions.filter { it.type == SessionRecord.TYPE_CAPACITY || it.type == SessionRecord.TYPE_RUNTIME }
                .mapNotNull { s -> s.metrics["runtimeS"]?.let { TrendChartView.Point(s.timestampMs, it / 60f) } }
        },
        Metric("OCP trip (A)", " A") { _, sessions ->
            sessions.filter { it.type == SessionRecord.TYPE_OCP }
                .mapNotNull { s -> s.metrics["tripA"]?.takeIf { it > 0f }
                    ?.let { TrendChartView.Point(s.timestampMs, it) } }
        },
    )

    private var tests: List<TestRecord> = emptyList()
    private var sessions: List<SessionRecord> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_trends)
        findViewById<MaterialToolbar>(R.id.toolbar).setNavigationOnClickListener { finish() }

        val group = findViewById<ChipGroup>(R.id.metricGroup)
        metrics.forEachIndexed { idx, m ->
            val chip = layoutInflater.inflate(R.layout.chip_mode, group, false) as Chip
            chip.text = m.label
            chip.id = android.view.View.generateViewId()
            chip.isCheckable = true
            chip.setOnClickListener { render(idx) }
            group.addView(chip)
            if (idx == 0) group.check(chip.id)
        }

        lifecycleScope.launch {
            val (t, s) = withContext(Dispatchers.IO) {
                TestRepository(this@TrendsActivity).list() to
                    SessionRepository(this@TrendsActivity).list()
            }
            tests = t.sortedBy { it.timestampMs }
            sessions = s.sortedBy { it.timestampMs }
            render(0)
        }
    }

    private fun render(metricIdx: Int) {
        val m = metrics[metricIdx]
        val pts = m.extract(tests, sessions)
        findViewById<TrendChartView>(R.id.trendChart).setData(pts, m.unit)
        val fmt = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault())
        val sb = StringBuilder("Date              ${m.label}\n")
        sb.append("--------------------------------\n")
        if (pts.isEmpty()) sb.append(getString(R.string.trends_empty))
        for (p in pts.sortedByDescending { it.timestampMs }) {
            sb.append("%s  %8.2f\n".format(fmt.format(Date(p.timestampMs)), p.value))
        }
        findViewById<android.widget.TextView>(R.id.trendTable).text = sb.toString().trimEnd()
    }
}
