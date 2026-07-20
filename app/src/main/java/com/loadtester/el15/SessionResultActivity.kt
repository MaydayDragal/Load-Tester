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
import androidx.lifecycle.lifecycleScope
import androidx.print.PrintHelper
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.google.android.material.chip.Chip
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.loadtester.el15.databinding.ActivitySessionResultBinding
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Results view for one archived bench session (battery capacity, runtime,
 * step-load, or OCP ramp): headline metric, battery-health grading, params,
 * tag/notes, an interactive zoom/pan/cursor time-series chart, and print /
 * share / CSV / PNG / PDF export of a blueprint-styled report.
 */
class SessionResultActivity : BaseActivity() {

    private lateinit var binding: ActivitySessionResultBinding
    private lateinit var repo: SessionRepository
    private var record: SessionRecord? = null

    private var pendingSave: (() -> Unit)? = null
    private val storagePermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) pendingSave?.invoke() else toast("Storage permission denied")
        pendingSave = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivitySessionResultBinding.inflate(layoutInflater)
        setContentView(binding.root)
        binding.toolbar.setNavigationOnClickListener { finish() }

        repo = SessionRepository(this)
        record = intent.getStringExtra(ResultActivity.EXTRA_RECORD_ID)?.let { repo.load(it) }
        val r = record
        if (r == null) {
            toast("Session record not found")
            finish()
            return
        }

        binding.typeHeader.text = r.typeName().uppercase(Locale.US)
        binding.headlineValue.text = r.headline()
        binding.summaryText.text = buildSummary(r)
        binding.metadataText.text = buildMetadata(r)
        binding.tagInput.setText(r.tag)
        binding.notesInput.setText(r.notes)
        bindHealth(r)

        binding.chart.setData(r.points)
        val chips = mapOf(binding.seriesV to 'V', binding.seriesI to 'I',
            binding.seriesP to 'P', binding.seriesT to 'T')
        for ((chip, key) in chips) {
            chip.isChecked = key in binding.chart.visible
            chip.setOnClickListener {
                binding.chart.toggleSeries(key)
                syncChips(chips)
            }
        }
        binding.resetZoom.setOnClickListener { binding.chart.resetView() }

        binding.printButton.setOnClickListener { printReport() }
        binding.shareButton.setOnClickListener { shareChooser() }
        binding.csvButton.setOnClickListener { exportCsv() }
        binding.saveButton.setOnClickListener { saveChooser() }
    }

    private fun syncChips(chips: Map<Chip, Char>) {
        for ((chip, key) in chips) chip.isChecked = key in binding.chart.visible
    }

    override fun onPause() {
        super.onPause()
        record?.let { r ->
            val tag = binding.tagInput.text?.toString()?.trim().orEmpty()
            val notes = binding.notesInput.text?.toString()?.trim().orEmpty()
            if (tag != r.tag || notes != r.notes) {
                r.tag = tag
                r.notes = notes
                repo.save(r)
            }
        }
    }

    // ---- Text builders ------------------------------------------------------
    private fun buildSummary(r: SessionRecord): String {
        val lines = ArrayList<String>()
        if (r.isPartial) lines += "⚠ Partial — the test was stopped or interrupted early."
        for ((k, v) in r.metrics) {
            if (k == "aborted") continue // surfaced as the partial banner above
            lines += "%-14s: %s".format(metricLabel(k), metricValue(k, v))
        }
        return lines.joinToString("\n")
    }

    private fun buildMetadata(r: SessionRecord): String {
        val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault())
            .format(Date(r.timestampMs))
        val lines = ArrayList<String>()
        lines += "Time      : $stamp"
        lines += "Device    : ${r.deviceLabel}"
        for ((k, v) in r.params) lines += "%-10s: %s".format(paramLabel(k), paramValue(k, v))
        lines += "Points    : ${r.points.size}"
        return lines.joinToString("\n")
    }

    private fun metricLabel(k: String): String = when (k) {
        "capacityAh" -> "Capacity"
        "energyWh" -> "Energy"
        "runtimeS" -> "Runtime"
        "sohPct" -> "State of health"
        "endV" -> "End voltage"
        "startV" -> "Start voltage"
        "droopV" -> "Worst droop"
        "vMin" -> "Min voltage"
        "recoveryMs" -> "Avg recovery"
        "cycles" -> "Cycles"
        "tripA" -> "Trip current"
        "maxA" -> "Max current"
        else -> k
    }

    private fun metricValue(k: String, v: Float): String = when (k) {
        "capacityAh" -> "%.3f Ah".format(v)
        "energyWh" -> "%.2f Wh".format(v)
        "runtimeS" -> SessionRecord.fmtDuration(v.toLong())
        "sohPct" -> "%.0f %%".format(v)
        "endV", "startV", "vMin" -> "%.3f V".format(v)
        "droopV" -> "%.0f mV".format(v * 1000f)
        "recoveryMs" -> "%.0f ms".format(v)
        "cycles" -> "%.0f".format(v)
        "tripA", "maxA" -> if (v > 0f) "%.3f A".format(v) else "—"
        else -> "%.3f".format(v)
    }

    private fun paramLabel(k: String): String = when (k) {
        "dischargeA" -> "Discharge"
        "cutoffV" -> "Cutoff"
        "ratedAh" -> "Rated"
        "cells" -> "Cells"
        "chem" -> "Chemistry"
        "powerW" -> "Power"
        "iLow" -> "I low"
        "iHigh" -> "I high"
        "periodMs" -> "Period"
        "cycles" -> "Cycles"
        "startA" -> "Start I"
        "stepA" -> "Step I"
        "dwellMs" -> "Dwell"
        "collapseFrac" -> "Collapse at"
        "fuseA" -> "Fuse"
        else -> k
    }

    private fun paramValue(k: String, v: Float): String = when (k) {
        "dischargeA", "iLow", "iHigh", "startA", "stepA", "fuseA" -> "%.3f A".format(v)
        "cutoffV" -> "%.2f V".format(v)
        "ratedAh" -> "%.2f Ah".format(v)
        "cells" -> "%.0f in series".format(v)
        "chem" -> LongTestEngine.CHEMISTRIES.getOrNull(v.toInt())?.name ?: "—"
        "powerW" -> "%.1f W".format(v)
        "periodMs", "dwellMs" -> "%.0f ms".format(v)
        "cycles" -> "%.0f".format(v)
        "collapseFrac" -> "%.0f %% of Voc".format(v * 100f)
        else -> "%.3f".format(v)
    }

    // ---- Battery health ------------------------------------------------------
    private data class Health(val grade: String, val color: Int, val lines: List<String>)

    /** Paired R-test, loaded off the main thread in [bindHealth]. */
    private var pairedRTest: TestRecord? = null

    private fun buildHealth(r: SessionRecord): Health? {
        if (r.type != SessionRecord.TYPE_CAPACITY) return null
        val soh = r.metrics["sohPct"] ?: return null
        if (soh <= 0f) return null
        val (grade, colorRes) = when {
            soh >= 90f -> "EXCELLENT" to R.color.value_green
            soh >= 80f -> "GOOD" to R.color.value_green
            soh >= 60f -> "FAIR" to R.color.value_amber
            else -> "REPLACE" to R.color.value_red
        }
        val lines = ArrayList<String>()
        lines += "Measured %.3f Ah of %.2f Ah rated (%.0f%%)".format(
            r.metrics["capacityAh"] ?: 0f, r.params["ratedAh"] ?: 0f, soh)
        // Pair with the most recent resistance test on record: internal
        // resistance + capacity together give the full health picture.
        val rTest = pairedRTest
        if (rTest != null) {
            lines += "Internal resistance %.1f mΩ (R-test %s)".format(
                rTest.resistanceOhm * 1000f,
                SimpleDateFormat("MM-dd HH:mm", Locale.getDefault()).format(Date(rTest.timestampMs)))
        } else {
            lines += "Run a resistance test within 48 h to add internal resistance here."
        }
        return Health(grade, ContextCompat.getColor(this, colorRes), lines)
    }

    private fun bindHealth(r: SessionRecord) {
        fun render() {
            val h = buildHealth(r) ?: run { binding.healthFrame.visibility = View.GONE; return }
            binding.healthFrame.visibility = View.VISIBLE
            binding.healthGrade.text = h.grade
            binding.healthGrade.setTextColor(h.color)
            binding.healthText.text = h.lines.joinToString("\n")
        }
        render()
        // The R-test pairing parses the whole test archive — do it off the
        // main thread, then re-render with the internal-resistance line.
        lifecycleScope.launch {
            pairedRTest = withContext(Dispatchers.IO) {
                TestRepository(this@SessionResultActivity).list().firstOrNull {
                    it.reliable && kotlin.math.abs(it.timestampMs - r.timestampMs) < 48L * 3600_000L
                }
            }
            if (pairedRTest != null) render()
        }
    }

    // ---- Report --------------------------------------------------------------
    private fun buildReportBitmap(): Bitmap {
        val r = record!!
        val w = 1080
        val margin = 40f
        val gap = 18f
        val framePad = 26f
        val headerBlock = 48f

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
            color = steel; textSize = 54f; typeface = Typeface.MONOSPACE; isFakeBoldText = true
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

        class Section(val header: String, val contentH: Float, val draw: (Canvas, Float, Float, Float) -> Unit) {
            val totalH: Float get() = framePad + headerBlock + contentH + framePad
        }
        val sections = ArrayList<Section>()

        fun textSection(header: String, lines: List<String>, paint: Paint, lineH: Float) {
            sections.add(Section(header, lines.size * lineH) { c, top, x, _ ->
                var y = top + lineH * 0.78f
                for (line in lines) { c.drawText(line, x, y, paint); y += lineH }
            })
        }

        // Headline + metrics.
        val summaryLines = buildSummary(r).split("\n")
        val bigLine = 66f
        sections.add(Section(r.typeName().uppercase(Locale.US),
            bigLine + 10f + summaryLines.size * 40f) { c, top, x, _ ->
            c.drawText(r.headline(), x, top + 50f, bigPaint)
            var y = top + bigLine + 10f + 40f * 0.7f
            for (line in summaryLines) { c.drawText(line, x, y, monoMuted); y += 40f }
        })

        // Battery health.
        buildHealth(r)?.let { h ->
            val body = h.lines.flatMap { wrapText(it, bodyPaint, contentW) }
            sections.add(Section("BATTERY HEALTH", 46f + body.size * 38f) { c, top, x, _ ->
                val gradePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                    color = h.color; textSize = 40f; typeface = condensed
                }
                c.drawText(h.grade, x, top + 36f, gradePaint)
                var y = top + 46f + 38f * 0.7f
                for (line in body) { c.drawText(line, x, y, bodyPaint); y += 38f }
            })
        }

        // Details.
        textSection("TEST DETAILS", buildMetadata(r).split("\n"), monoPaint, 34f)

        // Notes.
        val notes = binding.notesInput.text?.toString()?.trim().orEmpty()
        if (notes.isNotEmpty()) {
            textSection("NOTES", wrapText(notes, bodyPaint, contentW), bodyPaint, 38f)
        }

        // Chart — fixed scale so density can't distort the report.
        val chartH = 560f
        val reportScale = w / 393f
        sections.add(Section("TIME SERIES (FULL SPAN)", chartH) { c, top, x, cw ->
            binding.chart.drawInto(c, x, top, cw, chartH, light = true, scale = reportScale)
        })

        val titleBlock = 108f
        val totalH = (margin * 2 + titleBlock +
            sections.sumOf { (it.totalH + gap).toDouble() }).toInt().coerceAtLeast(300)
        val bmp = Bitmap.createBitmap(w, totalH, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        c.drawColor(Color.WHITE)

        var y = margin
        c.drawText(r.typeName().uppercase(Locale.US) + " RESULTS", margin, y + 44f, titlePaint)
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

    // ---- Actions --------------------------------------------------------------
    private fun baseName(): String {
        val r = record ?: return "el15-session"
        val kind = when (r.type) {
            SessionRecord.TYPE_CAPACITY -> "capacity"
            SessionRecord.TYPE_RUNTIME -> "runtime"
            SessionRecord.TYPE_STEP -> "step"
            SessionRecord.TYPE_OCP -> "ocp"
            else -> "session"
        }
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date(r.timestampMs))
        return "el15-$kind-$stamp"
    }

    private fun printReport() {
        val helper = PrintHelper(this).apply { scaleMode = PrintHelper.SCALE_MODE_FIT }
        helper.printBitmap("EL15 ${record?.typeName()}", buildReportBitmap())
    }

    private fun shareChooser() {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.rt_share)
            .setItems(arrayOf(getString(R.string.rt_save_report),
                getString(R.string.save_pdf), getString(R.string.rt_save_csv))) { _, which ->
                try {
                    when (which) {
                        0 -> Exporter.shareBitmap(this, "${baseName()}.png", buildReportBitmap(),
                            "EL15 ${record?.typeName()}")
                        1 -> Exporter.share(this, "${baseName()}.pdf", "application/pdf",
                            Exporter.pdfFromBitmap(buildReportBitmap()), "EL15 ${record?.typeName()}")
                        2 -> Exporter.share(this, "${baseName()}.csv", "text/csv",
                            record!!.toCsv().toByteArray(), "EL15 ${record?.typeName()} data")
                    }
                } catch (e: Exception) { toast("Share failed: ${e.message}") }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun exportCsv() {
        try {
            Exporter.share(this, "${baseName()}.csv", "text/csv",
                record!!.toCsv().toByteArray(), "EL15 ${record?.typeName()} data")
        } catch (e: Exception) { toast("Export failed: ${e.message}") }
    }

    private fun saveChooser() {
        val options = arrayOf(getString(R.string.rt_save_report), getString(R.string.save_pdf),
            getString(R.string.rt_save_csv), getString(R.string.rt_save_both))
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
        if (which == 0 || which == 3) {
            Exporter.saveBitmapToDownloads(this, "${baseName()}.png", buildReportBitmap())
                ?.let { results.add(it) } ?: run { failed = true }
        }
        if (which == 1) {
            try {
                Exporter.saveToDownloads(this, "${baseName()}.pdf", "application/pdf",
                    Exporter.pdfFromBitmap(buildReportBitmap()))
                    ?.let { results.add(it) } ?: run { failed = true }
            } catch (e: Exception) { failed = true }
        }
        if (which == 2 || which == 3) {
            Exporter.saveToDownloads(this, "${baseName()}.csv", "text/csv",
                record!!.toCsv().toByteArray())?.let { results.add(it) } ?: run { failed = true }
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
        fun start(from: Context, recordId: String) {
            from.startActivity(Intent(from, SessionResultActivity::class.java)
                .putExtra(ResultActivity.EXTRA_RECORD_ID, recordId))
        }
    }
}
