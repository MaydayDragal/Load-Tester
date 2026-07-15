package com.loadtester.el15

import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.view.View
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.FileProvider
import com.google.android.material.checkbox.MaterialCheckBox
import androidx.print.PrintHelper
import com.loadtester.el15.databinding.ActivityResultBinding
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Presents a completed circuit-resistance test: headline resistance, per-run
 * metrics (open-circuit voltage, peak power, unit temperature, fan speed, fit
 * quality), a V–I plot with the fitted line, a voltage/current trend plot, and
 * the raw data table. A "Report contents" checklist selects which of these are
 * rendered into the Print / Share report (a white-background bitmap).
 */
class ResultActivity : AppCompatActivity() {

    private lateinit var binding: ActivityResultBinding
    private var samples: List<CircuitResistanceTester.Sample> = emptyList()
    private var resistance = 0f
    private var vOc = 0f
    private var rSquared = 0f
    private var fuse = 0f
    private var maxCurrent = 0f
    private var reliable = false

    // Derived metrics
    private var peakPower = 0f
    private var maxTemp = 0f
    private var maxFan = 0

    /** Report sections the user can include/exclude (key, label, default-on). */
    private data class ReportItem(val key: String, val label: String, val default: Boolean)

    private val reportItems = listOf(
        ReportItem(KEY_RESISTANCE, "Circuit resistance", true),
        ReportItem(KEY_VOC, "Open-circuit voltage", true),
        ReportItem(KEY_FIT, "Fit quality (R²)", true),
        ReportItem(KEY_RANGE, "Test range & limit", true),
        ReportItem(KEY_POWER, "Peak power dissipated", true),
        ReportItem(KEY_TEMP, "Unit temperature", true),
        ReportItem(KEY_FAN, "Fan speed", true),
        ReportItem(KEY_VI, "Voltage–Current graph", true),
        ReportItem(KEY_TREND, "Voltage & Current trend graph", true),
        ReportItem(KEY_TABLE, "Data table", true),
    )
    private val selected = linkedSetOf<String>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityResultBinding.inflate(layoutInflater)
        setContentView(binding.root)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        title = getString(R.string.rt_results_title)

        val currents = intent.getFloatArrayExtra(EXTRA_CURRENTS) ?: FloatArray(0)
        val voltages = intent.getFloatArrayExtra(EXTRA_VOLTAGES) ?: FloatArray(0)
        val temps = intent.getFloatArrayExtra(EXTRA_TEMPS) ?: FloatArray(0)
        val fans = intent.getIntArrayExtra(EXTRA_FANS) ?: IntArray(0)
        samples = currents.indices.map {
            CircuitResistanceTester.Sample(
                currents[it],
                voltages.getOrElse(it) { 0f },
                temps.getOrElse(it) { 0f },
                fans.getOrElse(it) { 0 },
            )
        }
        resistance = intent.getFloatExtra(EXTRA_RESISTANCE, 0f)
        vOc = intent.getFloatExtra(EXTRA_VOC, 0f)
        rSquared = intent.getFloatExtra(EXTRA_R2, 0f)
        fuse = intent.getFloatExtra(EXTRA_FUSE, 0f)
        maxCurrent = intent.getFloatExtra(EXTRA_MAXI, 0f)
        reliable = intent.getBooleanExtra(EXTRA_RELIABLE, false)

        peakPower = samples.maxOfOrNull { it.power } ?: 0f
        maxTemp = samples.maxOfOrNull { it.temperature } ?: 0f
        maxFan = samples.maxOfOrNull { it.fanSpeed } ?: 0

        loadSelection()
        bind()
        buildReportChecklist()
        binding.printButton.setOnClickListener { printReport() }
        binding.shareButton.setOnClickListener { shareReport() }
    }

    override fun onSupportNavigateUp(): Boolean {
        finish()
        return true
    }

    private fun bind() {
        binding.resistanceValue.text = formatResistance(resistance)
        binding.summaryText.text = listOf(
            "Open-circuit voltage: %.3f V".format(vOc),
            "Tested up to %.2f A  (%.0f%% of %.1f A fuse)".format(maxCurrent, safePercent(), fuse),
            "Peak power dissipated: %.1f W".format(peakPower),
            "Max unit temperature: %.1f °C".format(maxTemp),
            "Max fan speed: %d / %d".format(maxFan, El15Protocol.FAN_SPEED_MAX),
            "Points: %d      Fit R²: %.4f".format(samples.size, rSquared),
        ).joinToString("\n")

        if (!reliable) {
            binding.reliabilityText.visibility = View.VISIBLE
            binding.reliabilityText.setTextColor(Color.parseColor("#FFB300"))
            binding.reliabilityText.text = getString(R.string.rt_low_confidence)
        }

        binding.viChart.mode = ResistanceChartView.MODE_VI
        binding.viChart.setData(samples, resistance, vOc)
        binding.trendChart.mode = ResistanceChartView.MODE_TREND
        binding.trendChart.setData(samples, resistance, vOc)

        binding.dataTable.text = buildTable()
    }

    private fun buildReportChecklist() {
        val container = binding.reportItemsContainer
        container.removeAllViews()
        for (item in reportItems) {
            val cb = MaterialCheckBox(this).apply {
                text = item.label
                isChecked = item.key in selected
                setOnCheckedChangeListener { _, checked ->
                    if (checked) selected.add(item.key) else selected.remove(item.key)
                    saveSelection()
                }
            }
            container.addView(cb)
        }
    }

    private fun safePercent(): Float = if (fuse > 0f) maxCurrent / fuse * 100f else 0f

    private fun buildTable(): String {
        val sb = StringBuilder()
        sb.append("Step   I(A)      V(V)      P(W)      T(°C)   Fan   R@pt(Ω)\n")
        sb.append("-------------------------------------------------------------\n")
        samples.forEachIndexed { idx, s ->
            val rPt = if (s.current > 1e-4f && vOc > 0f) (vOc - s.voltage) / s.current else 0f
            sb.append(
                "%-4d   %7.3f   %7.3f   %7.2f   %5.1f   %3d   %8.4f\n"
                    .format(idx + 1, s.current, s.voltage, s.power, s.temperature, s.fanSpeed, rPt)
            )
        }
        return sb.toString().trimEnd()
    }

    private fun formatResistance(ohm: Float): String = when {
        ohm <= 0f -> "n/a"
        ohm < 1f -> "%.1f mΩ".format(ohm * 1000f)
        else -> "%.3f Ω".format(ohm)
    }

    // ---- Selection persistence -------------------------------------------
    private fun loadSelection() {
        val prefs = getSharedPreferences("report", MODE_PRIVATE)
        val saved = prefs.getStringSet("items", null)
        selected.clear()
        if (saved == null) {
            reportItems.filter { it.default }.forEach { selected.add(it.key) }
        } else {
            selected.addAll(saved)
        }
    }

    private fun saveSelection() {
        getSharedPreferences("report", MODE_PRIVATE).edit()
            .putStringSet("items", selected).apply()
    }

    // ---- Report rendering -------------------------------------------------
    private fun sel(key: String) = key in selected

    private fun buildReportBitmap(): Bitmap {
        val w = 1080
        val margin = 48f
        val gap = 16f
        val chartW = w - margin * 2
        val chartH = 560f

        val title = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.parseColor("#102027"); textSize = 44f; isFakeBoldText = true
        }
        val body = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.parseColor("#37474F"); textSize = 30f
        }
        val caption = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.parseColor("#102027"); textSize = 30f; isFakeBoldText = true
        }
        val big = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.parseColor("#00838F"); textSize = 66f; isFakeBoldText = true
        }
        val mono = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.parseColor("#102027"); textSize = 24f; typeface = Typeface.MONOSPACE
        }

        val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault()).format(Date())

        // Build the ordered list of (height, draw) sections from the selection.
        val sections = ArrayList<Pair<Float, (Canvas, Float) -> Unit>>()

        // Header is always present.
        sections.add(96f to { c, top ->
            c.drawText(getString(R.string.rt_results_title), margin, top + 44f, title)
            c.drawText(stamp, margin, top + 84f, body)
        })
        if (sel(KEY_RESISTANCE)) sections.add(80f to { c, top ->
            c.drawText("Circuit resistance", margin, top + 26f, body)
            c.drawText(formatResistance(resistance), margin, top + 74f, big)
        })
        fun line(text: String) = 40f to { c: Canvas, top: Float -> c.drawText(text, margin, top + 30f, body) }
        if (sel(KEY_VOC)) sections.add(line("Open-circuit voltage: %.3f V".format(vOc)))
        if (sel(KEY_RANGE)) sections.add(line(
            "Tested up to %.2f A  (%.0f%% of %.1f A fuse)".format(maxCurrent, safePercent(), fuse)))
        if (sel(KEY_POWER)) sections.add(line("Peak power dissipated: %.1f W".format(peakPower)))
        if (sel(KEY_TEMP)) sections.add(line("Max unit temperature: %.1f °C".format(maxTemp)))
        if (sel(KEY_FAN)) sections.add(line("Max fan speed: %d / %d".format(maxFan, El15Protocol.FAN_SPEED_MAX)))
        if (sel(KEY_FIT)) sections.add(line(
            "Points: %d      Fit R²: %.4f%s".format(
                samples.size, rSquared, if (!reliable) "  (low confidence)" else "")))
        if (sel(KEY_VI)) sections.add((chartH + 44f) to { c, top ->
            c.drawText("Voltage vs Current (slope = resistance)", margin, top + 28f, caption)
            binding.viChart.drawChart(c, margin, top + 40f, chartW, chartH, light = true)
        })
        if (sel(KEY_TREND)) sections.add((chartH + 44f) to { c, top ->
            c.drawText("Voltage & Current per step", margin, top + 28f, caption)
            binding.trendChart.drawChart(c, margin, top + 40f, chartW, chartH, light = true)
        })
        if (sel(KEY_TABLE)) {
            val lines = buildTable().split("\n")
            val tableH = 44f + lines.size * 32f
            sections.add(tableH to { c, top ->
                c.drawText("Measurements", margin, top + 28f, caption)
                var ty = top + 62f
                for (l in lines) { c.drawText(l, margin, ty, mono); ty += 32f }
            })
        }

        val totalH = (margin * 2 + sections.sumOf { it.first.toDouble() } +
            gap * (sections.size - 1).coerceAtLeast(0)).toInt().coerceAtLeast(200)

        val bmp = Bitmap.createBitmap(w, totalH, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        c.drawColor(Color.WHITE)
        var y = margin
        for ((height, draw) in sections) {
            draw(c, y)
            y += height + gap
        }
        return bmp
    }

    private fun printReport() {
        val helper = PrintHelper(this).apply { scaleMode = PrintHelper.SCALE_MODE_FIT }
        helper.printBitmap("EL15 Resistance Test", buildReportBitmap())
    }

    private fun shareReport() {
        try {
            val dir = File(cacheDir, "shared").apply { mkdirs() }
            val file = File(dir, "resistance-test.png")
            FileOutputStream(file).use { buildReportBitmap().compress(Bitmap.CompressFormat.PNG, 100, it) }
            val uri: Uri = FileProvider.getUriForFile(this, "$packageName.fileprovider", file)
            val send = Intent(Intent.ACTION_SEND).apply {
                type = "image/png"
                putExtra(Intent.EXTRA_STREAM, uri)
                putExtra(Intent.EXTRA_SUBJECT, "Circuit resistance: ${formatResistance(resistance)}")
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            startActivity(Intent.createChooser(send, getString(R.string.rt_share)))
        } catch (e: Exception) {
            android.widget.Toast.makeText(this, "Share failed: ${e.message}", android.widget.Toast.LENGTH_LONG).show()
        }
    }

    companion object {
        const val EXTRA_CURRENTS = "currents"
        const val EXTRA_VOLTAGES = "voltages"
        const val EXTRA_TEMPS = "temps"
        const val EXTRA_FANS = "fans"
        const val EXTRA_RESISTANCE = "resistance"
        const val EXTRA_VOC = "voc"
        const val EXTRA_R2 = "r2"
        const val EXTRA_FUSE = "fuse"
        const val EXTRA_MAXI = "maxi"
        const val EXTRA_RELIABLE = "reliable"

        private const val KEY_RESISTANCE = "resistance"
        private const val KEY_VOC = "voc"
        private const val KEY_FIT = "fit"
        private const val KEY_RANGE = "range"
        private const val KEY_POWER = "power"
        private const val KEY_TEMP = "temp"
        private const val KEY_FAN = "fan"
        private const val KEY_VI = "vi"
        private const val KEY_TREND = "trend"
        private const val KEY_TABLE = "table"

        fun start(from: AppCompatActivity, result: CircuitResistanceTester.ResistanceResult) {
            val s = result.samples
            val i = Intent(from, ResultActivity::class.java).apply {
                putExtra(EXTRA_CURRENTS, FloatArray(s.size) { s[it].current })
                putExtra(EXTRA_VOLTAGES, FloatArray(s.size) { s[it].voltage })
                putExtra(EXTRA_TEMPS, FloatArray(s.size) { s[it].temperature })
                putExtra(EXTRA_FANS, IntArray(s.size) { s[it].fanSpeed })
                putExtra(EXTRA_RESISTANCE, result.resistanceOhm)
                putExtra(EXTRA_VOC, result.openCircuitVoltage)
                putExtra(EXTRA_R2, result.rSquared)
                putExtra(EXTRA_FUSE, result.fuseRating)
                putExtra(EXTRA_MAXI, result.maxTestCurrent)
                putExtra(EXTRA_RELIABLE, result.reliable)
            }
            from.startActivity(i)
        }
    }
}
