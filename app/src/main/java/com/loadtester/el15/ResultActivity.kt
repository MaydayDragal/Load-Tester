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
class ResultActivity : BaseActivity() {

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

    // Test metadata
    private var steps = 0
    private var settleMs = 0L
    private var sampleMs = 0L
    private var safetyPct = 80
    private var deviceLabel = ""
    private var timestampMs = 0L

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
        ReportItem(KEY_META, "Test details", true),
        ReportItem(KEY_NOTES, "Notes", true),
    )
    private val selected = linkedSetOf<String>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityResultBinding.inflate(layoutInflater)
        setContentView(binding.root)
        binding.toolbar.setNavigationOnClickListener { finish() }

        steps = intent.getIntExtra(EXTRA_STEPS, 0)
        settleMs = intent.getLongExtra(EXTRA_SETTLE, 0)
        sampleMs = intent.getLongExtra(EXTRA_SAMPLE, 0)
        safetyPct = intent.getIntExtra(EXTRA_SAFETY, 80)
        deviceLabel = intent.getStringExtra(EXTRA_DEVICE) ?: "EL15"
        timestampMs = intent.getLongExtra(EXTRA_TIME, 0L)

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
        binding.csvButton.setOnClickListener { exportCsv() }
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
        binding.metadataText.text = buildMetadata()
    }

    private fun buildMetadata(): String {
        val when_ = if (timestampMs > 0)
            SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault()).format(Date(timestampMs))
        else "—"
        val durS = steps * (settleMs + sampleMs) / 1000.0
        return listOf(
            "Time      : $when_",
            "Device    : $deviceLabel",
            "Fuse      : %.1f A   (safety %d%%)".format(fuse, safetyPct),
            "Peak test : %.2f A".format(maxCurrent),
            "Sweep     : %d steps, %d ms settle, %d ms sample".format(steps, settleMs, sampleMs),
            "Duration  : ~%.0f s".format(durS),
        ).joinToString("\n")
    }

    private fun notesText(): String = binding.notesInput.text?.toString()?.trim().orEmpty()

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
        if (sel(KEY_META)) {
            val lines = buildMetadata().split("\n")
            sections.add((44f + lines.size * 30f) to { c, top ->
                c.drawText("Test details", margin, top + 28f, caption)
                var ty = top + 60f
                for (l in lines) { c.drawText(l, margin, ty, mono); ty += 30f }
            })
        }
        if (sel(KEY_VI)) sections.add((chartH + 44f) to { c, top ->
            c.drawText("Voltage vs Current (slope = resistance)", margin, top + 28f, caption)
            binding.viChart.drawChart(c, margin, top + 40f, chartW, chartH, light = true)
        })
        if (sel(KEY_TREND)) sections.add((chartH + 44f) to { c, top ->
            c.drawText("Metrics per step — V, I, P, temp, fan (normalized)", margin, top + 28f, caption)
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
        if (sel(KEY_NOTES) && notesText().isNotEmpty()) {
            val wrapped = wrapText(notesText(), body, chartW)
            sections.add((44f + wrapped.size * 34f) to { c, top ->
                c.drawText("Notes", margin, top + 28f, caption)
                var ty = top + 60f
                for (l in wrapped) { c.drawText(l, margin, ty, body); ty += 34f }
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

    private fun wrapText(text: String, paint: Paint, maxW: Float): List<String> {
        val out = ArrayList<String>()
        for (para in text.split("\n")) {
            var line = ""
            for (word in para.split(" ")) {
                val trial = if (line.isEmpty()) word else "$line $word"
                if (paint.measureText(trial) > maxW && line.isNotEmpty()) {
                    out.add(line); line = word
                } else line = trial
            }
            out.add(line)
        }
        return out
    }

    private fun exportCsv() {
        try {
            val dir = File(cacheDir, "shared").apply { mkdirs() }
            val file = File(dir, "resistance-test.csv")
            FileOutputStream(file).use { out ->
                val sb = StringBuilder()
                sb.append("# EL15 circuit resistance test\n")
                sb.append("# ").append(buildMetadata().replace("\n", "\n# ")).append("\n")
                sb.append("# resistance_ohm,%.6f\n".format(resistance))
                sb.append("# open_circuit_v,%.4f\n".format(vOc))
                sb.append("# r_squared,%.6f\n".format(rSquared))
                if (notesText().isNotEmpty()) sb.append("# notes,").append(notesText().replace("\n", " ")).append("\n")
                sb.append("step,current_A,voltage_V,power_W,temp_C,fan,resistance_at_point_ohm\n")
                samples.forEachIndexed { idx, s ->
                    val rPt = if (s.current > 1e-4f && vOc > 0f) (vOc - s.voltage) / s.current else 0f
                    sb.append("%d,%.4f,%.4f,%.4f,%.2f,%d,%.4f\n".format(
                        idx + 1, s.current, s.voltage, s.power, s.temperature, s.fanSpeed, rPt))
                }
                out.write(sb.toString().toByteArray())
            }
            val uri = FileProvider.getUriForFile(this, "$packageName.fileprovider", file)
            val send = Intent(Intent.ACTION_SEND).apply {
                type = "text/csv"
                putExtra(Intent.EXTRA_STREAM, uri)
                putExtra(Intent.EXTRA_SUBJECT, "EL15 resistance test data")
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            startActivity(Intent.createChooser(send, getString(R.string.rt_csv)))
        } catch (e: Exception) {
            toast("Export failed: ${e.message}")
        }
    }

    private fun toast(m: String) =
        android.widget.Toast.makeText(this, m, android.widget.Toast.LENGTH_LONG).show()

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
        const val EXTRA_STEPS = "steps"
        const val EXTRA_SETTLE = "settle"
        const val EXTRA_SAMPLE = "sample"
        const val EXTRA_SAFETY = "safety"
        const val EXTRA_DEVICE = "device"
        const val EXTRA_TIME = "time"

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
        private const val KEY_META = "meta"
        private const val KEY_NOTES = "notes"

        fun start(
            from: AppCompatActivity,
            result: CircuitResistanceTester.ResistanceResult,
            steps: Int,
            settleMs: Long,
            sampleMs: Long,
            safetyPct: Int,
            deviceLabel: String,
            timestampMs: Long,
        ) {
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
                putExtra(EXTRA_STEPS, steps)
                putExtra(EXTRA_SETTLE, settleMs)
                putExtra(EXTRA_SAMPLE, sampleMs)
                putExtra(EXTRA_SAFETY, safetyPct)
                putExtra(EXTRA_DEVICE, deviceLabel)
                putExtra(EXTRA_TIME, timestampMs)
            }
            from.startActivity(i)
        }
    }
}
