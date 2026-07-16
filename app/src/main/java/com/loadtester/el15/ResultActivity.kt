package com.loadtester.el15

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.os.Bundle
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import com.google.android.material.checkbox.MaterialCheckBox
import com.google.android.material.chip.Chip
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import androidx.print.PrintHelper
import com.loadtester.el15.databinding.ActivityResultBinding
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Full results view for one archived test: headline resistance, metadata,
 * persistent notes, an interactive multi-view chart (V–I fit, per-step trends,
 * R-linearity, power curve — with series/style controls), the data table, and
 * a selectable report that can be printed, shared, exported as CSV, or saved
 * straight to the phone's Downloads folder.
 *
 * Records are loaded from [TestRepository] by id — every completed sweep is
 * auto-archived, so this screen also serves recall from History.
 */
class ResultActivity : BaseActivity() {

    private lateinit var binding: ActivityResultBinding
    private lateinit var repo: TestRepository
    private var record: TestRecord? = null
    private var chartConfig = TestChartView.ChartConfig()

    /** Report sections the user can include/exclude (key, label). */
    private data class ReportItem(val key: String, val label: String)

    private val reportItems = listOf(
        ReportItem(KEY_RESISTANCE, "Summary & resistance"),
        ReportItem(KEY_META, "Test details"),
        ReportItem(KEY_VI, "Voltage–current graph"),
        ReportItem(KEY_TREND, "Trend graph"),
        ReportItem(KEY_CUSTOM, "Current chart view (as configured)"),
        ReportItem(KEY_TABLE, "Data table"),
        ReportItem(KEY_NOTES, "Notes"),
    )
    private val selected = linkedSetOf<String>()

    private var pendingSave: (() -> Unit)? = null
    private val storagePermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) pendingSave?.invoke() else toast("Storage permission denied")
        pendingSave = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityResultBinding.inflate(layoutInflater)
        setContentView(binding.root)
        binding.toolbar.setNavigationOnClickListener { finish() }

        repo = TestRepository(this)
        val id = intent.getStringExtra(EXTRA_RECORD_ID)
        record = id?.let { repo.load(it) }
        if (record == null) {
            toast("Test record not found")
            finish()
            return
        }

        chartConfig = TestChartView.ChartConfig.decode(
            getSharedPreferences("report", MODE_PRIVATE).getString("chart_config", null)
        )
        loadSelection()
        bind()
        setupChartControls()
        buildReportChecklist()

        binding.printButton.setOnClickListener { printReport() }
        binding.shareButton.setOnClickListener { shareReport() }
        binding.csvButton.setOnClickListener { exportCsvChooser() }
        binding.saveButton.setOnClickListener { saveChooser() }
    }

    override fun onPause() {
        super.onPause()
        // Persist notes into the archived record.
        record?.let { r ->
            val notes = notesText()
            if (notes != r.notes) {
                r.notes = notes
                repo.save(r)
            }
        }
        getSharedPreferences("report", MODE_PRIVATE).edit()
            .putString("chart_config", chartConfig.encode()).apply()
    }

    // ---- Binding -----------------------------------------------------------
    private fun bind() {
        val r = record ?: return
        binding.resistanceValue.text = formatResistance(r.resistanceOhm)
        binding.summaryText.text = listOf(
            "Open-circuit voltage: %.3f V".format(r.openCircuitVoltage),
            "Tested up to %.2f A  (%d%% of %.1f A fuse)".format(r.maxTestCurrent, r.safetyPct, r.fuseRating),
            "Peak power dissipated: %.1f W".format(r.peakPower),
            "Max unit temperature: %.1f °C".format(r.maxTemp),
            "Max fan speed: %d / %d".format(r.maxFan, El15Protocol.FAN_SPEED_MAX),
            "Points: %d      Fit R²: %.4f".format(r.samples.size, r.rSquared),
        ).joinToString("\n")

        if (!r.reliable) {
            binding.reliabilityText.visibility = View.VISIBLE
            binding.reliabilityText.setTextColor(ContextCompat.getColor(this, R.color.value_amber))
            binding.reliabilityText.text = getString(R.string.rt_low_confidence)
        }

        binding.chart.setData(r.samples, r.resistanceOhm, r.openCircuitVoltage)
        binding.chart.config = chartConfig
        binding.dataTable.text = buildTable()
        binding.metadataText.text = buildMetadata()
        binding.notesInput.setText(r.notes)
    }

    // ---- Chart controls ----------------------------------------------------
    private fun setupChartControls() {
        val viewChips = mapOf(
            binding.viewVi.id to TestChartView.ChartConfig.VIEW_VI,
            binding.viewTrend.id to TestChartView.ChartConfig.VIEW_TREND,
            binding.viewAbs.id to TestChartView.ChartConfig.VIEW_ABS,
            binding.viewRi.id to TestChartView.ChartConfig.VIEW_R_I,
            binding.viewPi.id to TestChartView.ChartConfig.VIEW_P_I,
        )
        // Restore selection.
        viewChips.entries.firstOrNull { it.value == chartConfig.view }?.let {
            binding.chartViewGroup.check(it.key)
        } ?: binding.chartViewGroup.check(binding.viewVi.id)
        updateSeriesVisibility()

        for ((chipId, mode) in viewChips) {
            findViewById<Chip>(chipId).setOnClickListener {
                chartConfig.view = mode
                updateSeriesVisibility()
                applyChart()
            }
        }

        val seriesChips = mapOf(
            binding.seriesV.id to 'V', binding.seriesI.id to 'I', binding.seriesP.id to 'P',
            binding.seriesT.id to 'T', binding.seriesF.id to 'F',
        )
        for ((chipId, key) in seriesChips) {
            val chip = findViewById<Chip>(chipId)
            chip.isChecked = key in chartConfig.series
            chip.setOnClickListener {
                if (chip.isChecked) chartConfig.series.add(key) else chartConfig.series.remove(key)
                if (chartConfig.series.isEmpty()) { // keep at least one series
                    chartConfig.series.add(key); chip.isChecked = true
                }
                applyChart()
            }
        }

        fun styleChip(chip: Chip, get: () -> Boolean, set: (Boolean) -> Unit) {
            chip.isChecked = get()
            chip.setOnClickListener { set(chip.isChecked); applyChart() }
        }
        styleChip(binding.styleGrid, { chartConfig.grid }, { chartConfig.grid = it })
        styleChip(binding.styleMarkers, { chartConfig.markers }, { chartConfig.markers = it })
        styleChip(binding.styleFill, { chartConfig.fill }, { chartConfig.fill = it })
        styleChip(binding.styleThick, { chartConfig.thick }, { chartConfig.thick = it })
    }

    private fun updateSeriesVisibility() {
        val trendish = chartConfig.view == TestChartView.ChartConfig.VIEW_TREND ||
            chartConfig.view == TestChartView.ChartConfig.VIEW_ABS
        binding.seriesGroup.visibility = if (trendish) View.VISIBLE else View.GONE
    }

    private fun applyChart() {
        binding.chart.config = chartConfig
    }

    // ---- Text builders -----------------------------------------------------
    private fun buildMetadata(): String {
        val r = record ?: return ""
        val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault()).format(Date(r.timestampMs))
        val durS = r.steps * (r.settleMs + r.sampleMs) / 1000.0
        return listOf(
            "Time      : $stamp",
            "Device    : ${r.deviceLabel}",
            "Fuse      : %.1f A   (safety %d%%)".format(r.fuseRating, r.safetyPct),
            "Peak test : %.2f A".format(r.maxTestCurrent),
            "Sweep     : %d steps, %d ms settle, %d ms sample".format(r.steps, r.settleMs, r.sampleMs),
            "Duration  : ~%.0f s".format(durS),
        ).joinToString("\n")
    }

    private fun notesText(): String = binding.notesInput.text?.toString()?.trim().orEmpty()

    private fun buildTable(): String {
        val r = record ?: return ""
        val sb = StringBuilder()
        sb.append("Step   I(A)      V(V)      P(W)      T(°C)   Fan   R@pt(Ω)\n")
        sb.append("-------------------------------------------------------------\n")
        r.samples.forEachIndexed { idx, s ->
            val rPt = if (s.current > 1e-4f && r.openCircuitVoltage > 0f)
                (r.openCircuitVoltage - s.voltage) / s.current else 0f
            sb.append("%-4d   %7.3f   %7.3f   %7.2f   %5.1f   %3d   %8.4f\n"
                .format(idx + 1, s.current, s.voltage, s.power, s.temperature, s.fanSpeed, rPt))
        }
        return sb.toString().trimEnd()
    }

    private fun formatResistance(ohm: Float): String = when {
        ohm <= 0f -> "n/a"
        ohm < 1f -> "%.1f mΩ".format(ohm * 1000f)
        else -> "%.3f Ω".format(ohm)
    }

    private fun buildCsv(): String {
        val r = record ?: return ""
        val sb = StringBuilder()
        sb.append("# EL15 circuit resistance test\n")
        sb.append("# ").append(buildMetadata().replace("\n", "\n# ")).append("\n")
        sb.append("# resistance_ohm,%.6f\n".format(Locale.US, r.resistanceOhm))
        sb.append("# open_circuit_v,%.4f\n".format(Locale.US, r.openCircuitVoltage))
        sb.append("# r_squared,%.6f\n".format(Locale.US, r.rSquared))
        if (notesText().isNotEmpty()) sb.append("# notes,").append(notesText().replace("\n", " ")).append("\n")
        sb.append("step,current_A,voltage_V,power_W,temp_C,fan,resistance_at_point_ohm\n")
        r.samples.forEachIndexed { idx, s ->
            val rPt = if (s.current > 1e-4f && r.openCircuitVoltage > 0f)
                (r.openCircuitVoltage - s.voltage) / s.current else 0f
            sb.append("%d,%.4f,%.4f,%.4f,%.2f,%d,%.4f\n".format(
                Locale.US, idx + 1, s.current, s.voltage, s.power, s.temperature, s.fanSpeed, rPt))
        }
        return sb.toString()
    }

    private fun baseName(): String {
        val r = record ?: return "el15-test"
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date(r.timestampMs))
        return "el15-test-$stamp"
    }

    // ---- Selection persistence ---------------------------------------------
    private fun loadSelection() {
        val saved = getSharedPreferences("report", MODE_PRIVATE).getStringSet("items_v2", null)
        selected.clear()
        if (saved == null) reportItems.forEach { selected.add(it.key) } else selected.addAll(saved)
    }

    private fun saveSelection() {
        getSharedPreferences("report", MODE_PRIVATE).edit()
            .putStringSet("items_v2", selected.toSet()).apply()
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

    // ---- Report rendering ---------------------------------------------------
    private fun sel(key: String) = key in selected

    private fun buildReportBitmap(): Bitmap {
        val r = record!!
        val w = 1080
        val margin = 40f
        val gap = 18f
        val framePad = 26f
        val headerBlock = 48f

        // Print-friendly ink mapping of the app's blueprint palette.
        val ink = Color.parseColor("#101821")
        val muted = Color.parseColor("#5A6774")
        val steel = Color.parseColor("#3C6188")
        val frameLine = Color.parseColor("#B9C4CF")
        val cornerInk = Color.parseColor("#8791A0")

        val condensed = Typeface.create("sans-serif-condensed", Typeface.BOLD)
        val titlePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = ink; textSize = 46f; typeface = condensed; letterSpacing = 0.04f
        }
        val kickerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = steel; textSize = 20f; typeface = condensed; letterSpacing = 0.16f
        }
        val headerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = muted; textSize = 26f; typeface = condensed; letterSpacing = 0.10f
        }
        val bigPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = steel; textSize = 64f; typeface = Typeface.MONOSPACE; isFakeBoldText = true
        }
        val monoPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = ink; textSize = 24f; typeface = Typeface.MONOSPACE
        }
        val monoMuted = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = muted; textSize = 26f; typeface = Typeface.MONOSPACE
        }
        val bodyPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = ink; textSize = 28f }
        val framePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = frameLine; style = Paint.Style.STROKE; strokeWidth = 2f
        }
        val cornerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = cornerInk; style = Paint.Style.STROKE; strokeWidth = 2f
        }

        val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault()).format(Date(r.timestampMs))
        val contentW = w - margin * 2 - framePad * 2

        /** A blueprint-framed section: uppercase header + content, like the app's cards. */
        class Section(val header: String, val contentH: Float, val draw: (Canvas, Float, Float, Float) -> Unit) {
            val totalH: Float get() = framePad + headerBlock + contentH + framePad
        }

        val sections = ArrayList<Section>()

        fun textSection(header: String, lines: List<Pair<String, Paint>>, lineH: Float) {
            sections.add(Section(header, lines.size * lineH) { c, top, x, _ ->
                var y = top + lineH * 0.78f
                for ((line, paint) in lines) { c.drawText(line, x, y, paint); y += lineH }
            })
        }

        // 1) Summary & resistance — mirrors the app's headline card.
        if (sel(KEY_RESISTANCE)) {
            val summary = listOf(
                "Open-circuit voltage: %.3f V".format(r.openCircuitVoltage),
                "Tested up to %.2f A  (%d%% of %.1f A fuse)".format(r.maxTestCurrent, r.safetyPct, r.fuseRating),
                "Peak power dissipated: %.1f W".format(r.peakPower),
                "Max unit temperature: %.1f °C".format(r.maxTemp),
                "Max fan speed: %d / %d".format(r.maxFan, El15Protocol.FAN_SPEED_MAX),
                "Points: %d      Fit R²: %.4f%s".format(
                    r.samples.size, r.rSquared, if (!r.reliable) "  (LOW CONFIDENCE)" else ""),
            )
            val bigLine = 78f
            val lineH = 40f
            sections.add(Section("CIRCUIT RESISTANCE", bigLine + 10f + summary.size * lineH) { c, top, x, _ ->
                // Big value first; its baseline sits a full ascent below the top,
                // so it can never collide with the section header above.
                c.drawText(formatResistance(r.resistanceOhm), x, top + 58f, bigPaint)
                var y = top + bigLine + 10f + lineH * 0.7f
                for (line in summary) { c.drawText(line, x, y, monoMuted); y += lineH }
            })
        }

        // 2) Test details.
        if (sel(KEY_META)) {
            textSection("TEST DETAILS", buildMetadata().split("\n").map { it to monoPaint }, 34f)
        }

        // 3) Notes — third, matching the app's card order.
        if (sel(KEY_NOTES) && notesText().isNotEmpty()) {
            textSection("NOTES", wrapText(notesText(), bodyPaint, contentW).map { it to bodyPaint }, 38f)
        }

        // 4) Graphs. Pin chart geometry to the report's fixed pixel width so
        // device density/font-scale cannot squash or clip the rendering.
        val chartH = 560f
        val reportScale = w / 393f
        fun chartSection(titleStr: String, cfg: TestChartView.ChartConfig) {
            sections.add(Section(titleStr, chartH) { c, top, x, cw ->
                binding.chart.drawChart(c, x, top, cw, chartH, light = true, cfg = cfg, scale = reportScale)
            })
        }
        if (sel(KEY_VI)) chartSection(
            "VOLTAGE VS CURRENT (SLOPE = RESISTANCE)",
            TestChartView.ChartConfig(view = TestChartView.ChartConfig.VIEW_VI,
                grid = chartConfig.grid, markers = true, thick = chartConfig.thick),
        )
        if (sel(KEY_TREND)) chartSection(
            "METRICS PER STEP (NORMALIZED)",
            TestChartView.ChartConfig(view = TestChartView.ChartConfig.VIEW_TREND,
                grid = chartConfig.grid, markers = chartConfig.markers,
                fill = chartConfig.fill, thick = chartConfig.thick),
        )
        val dupVi = chartConfig.view == TestChartView.ChartConfig.VIEW_VI && sel(KEY_VI)
        val dupTrend = chartConfig.view == TestChartView.ChartConfig.VIEW_TREND && sel(KEY_TREND)
        if (sel(KEY_CUSTOM) && !dupVi && !dupTrend) {
            val name = when (chartConfig.view) {
                TestChartView.ChartConfig.VIEW_VI -> "VOLTAGE VS CURRENT (SLOPE = RESISTANCE)"
                TestChartView.ChartConfig.VIEW_TREND -> "METRICS PER STEP (NORMALIZED)"
                TestChartView.ChartConfig.VIEW_ABS -> "METRICS PER STEP (ABSOLUTE)"
                TestChartView.ChartConfig.VIEW_R_I -> "RESISTANCE LINEARITY (R AT POINT VS CURRENT)"
                else -> "POWER VS CURRENT"
            }
            chartSection(name, chartConfig)
        }

        // 5) Data table — capped so a 1000-step sweep can't inflate the report
        // bitmap past sane memory; the CSV export always has every row.
        if (sel(KEY_TABLE)) {
            val allRows = buildTable().split("\n")
            val rows = if (allRows.size > REPORT_TABLE_MAX_ROWS + 2) {
                allRows.take(REPORT_TABLE_MAX_ROWS + 2) +
                    "… ${allRows.size - REPORT_TABLE_MAX_ROWS - 2} more rows — full data in the CSV export"
            } else allRows
            textSection("MEASUREMENTS", rows.map { it to monoPaint }, 32f)
        }

        // ---- Compose the page ----
        val titleBlock = 108f
        val totalH = (margin * 2 + titleBlock +
            sections.sumOf { (it.totalH + gap).toDouble() }).toInt().coerceAtLeast(300)
        val bmp = Bitmap.createBitmap(w, totalH, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        c.drawColor(Color.WHITE)

        // Page header, styled like the app's top bar.
        var y = margin
        c.drawText("RESISTANCE TEST RESULTS", margin, y + 44f, titlePaint)
        c.drawText("EL15 LOAD CONTROL · $stamp", margin, y + 82f, kickerPaint)
        y += titleBlock

        fun drawCorners(left: Float, top: Float, right: Float, bottom: Float) {
            val inset = 10f
            val half = 6f
            for ((cx, cy) in listOf(
                left + inset to top + inset, right - inset to top + inset,
                left + inset to bottom - inset, right - inset to bottom - inset,
            )) {
                c.drawLine(cx - half, cy, cx + half, cy, cornerPaint)
                c.drawLine(cx, cy - half, cx, cy + half, cornerPaint)
            }
        }

        for (s0 in sections) {
            val top = y
            val bottom = y + s0.totalH
            c.drawRect(margin, top, w - margin, bottom, framePaint)
            drawCorners(margin, top, w - margin, bottom)
            val x = margin + framePad
            c.drawText(s0.header, x, top + framePad + 20f, headerPaint)
            s0.draw(c, top + framePad + headerBlock, x, contentW)
            y = bottom + gap
        }
        return bmp
    }

    private fun wrapText(textStr: String, paint: Paint, maxW: Float): List<String> {
        val out = ArrayList<String>()
        for (para in textStr.split("\n")) {
            var line = ""
            for (word in para.split(" ")) {
                val trial = if (line.isEmpty()) word else "$line $word"
                if (paint.measureText(trial) > maxW && line.isNotEmpty()) { out.add(line); line = word }
                else line = trial
            }
            out.add(line)
        }
        return out
    }

    // ---- Actions ------------------------------------------------------------
    private fun printReport() {
        val helper = PrintHelper(this).apply { scaleMode = PrintHelper.SCALE_MODE_FIT }
        helper.printBitmap("EL15 Resistance Test", buildReportBitmap())
    }

    private fun shareReport() {
        try {
            Exporter.shareBitmap(this, "${baseName()}.png", buildReportBitmap(),
                "Circuit resistance: ${formatResistance(record?.resistanceOhm ?: 0f)}")
        } catch (e: Exception) { toast("Share failed: ${e.message}") }
    }

    private fun exportCsvChooser() {
        try {
            Exporter.share(this, "${baseName()}.csv", "text/csv",
                buildCsv().toByteArray(), "EL15 resistance test data")
        } catch (e: Exception) { toast("Export failed: ${e.message}") }
    }

    /** Save report PNG and/or CSV straight into the phone's Downloads folder. */
    private fun saveChooser() {
        val options = arrayOf(
            getString(R.string.rt_save_report),
            getString(R.string.rt_save_csv),
            getString(R.string.rt_save_both),
        )
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.rt_save_device)
            .setItems(options) { _, which -> withStoragePermission { doSave(which) } }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun withStoragePermission(action: () -> Unit) {
        if (!Exporter.needsLegacyWritePermission() ||
            ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
            PackageManager.PERMISSION_GRANTED
        ) {
            action()
        } else {
            pendingSave = action
            storagePermission.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        }
    }

    private fun doSave(which: Int) {
        val results = ArrayList<String>()
        var failed = false
        if (which == 0 || which == 2) {
            Exporter.saveBitmapToDownloads(this, "${baseName()}.png", buildReportBitmap())
                ?.let { results.add(it) } ?: run { failed = true }
        }
        if (which == 1 || which == 2) {
            Exporter.saveToDownloads(this, "${baseName()}.csv", "text/csv", buildCsv().toByteArray())
                ?.let { results.add(it) } ?: run { failed = true }
        }
        when {
            results.isNotEmpty() && !failed -> toast(getString(R.string.rt_saved_to, results.joinToString(", ")))
            results.isNotEmpty() -> toast("Partially saved: ${results.joinToString(", ")}")
            else -> toast("Save failed")
        }
    }

    private fun toast(m: String) =
        android.widget.Toast.makeText(this, m, android.widget.Toast.LENGTH_LONG).show()

    companion object {
        const val EXTRA_RECORD_ID = "record_id"
        private const val REPORT_TABLE_MAX_ROWS = 100

        private const val KEY_RESISTANCE = "resistance"
        private const val KEY_META = "meta"
        private const val KEY_VI = "vi"
        private const val KEY_TREND = "trend"
        private const val KEY_CUSTOM = "custom"
        private const val KEY_TABLE = "table"
        private const val KEY_NOTES = "notes"

        /** Archive the result, then open it. Returns false if the save failed. */
        fun start(from: Context, record: TestRecord): Boolean {
            if (!TestRepository(from).save(record)) return false
            from.startActivity(Intent(from, ResultActivity::class.java).apply {
                putExtra(EXTRA_RECORD_ID, record.id)
            })
            return true
        }
    }
}
