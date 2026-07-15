package com.loadtester.el15

import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.net.Uri
import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.FileProvider
import androidx.print.PrintHelper
import com.loadtester.el15.databinding.ActivityResultBinding
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Presents a completed circuit-resistance test: headline resistance, a V–I plot
 * with the fitted line, a voltage/current trend plot, the raw data table, and
 * Print / Share actions that render a white-background report.
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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityResultBinding.inflate(layoutInflater)
        setContentView(binding.root)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        title = getString(R.string.rt_results_title)

        val currents = intent.getFloatArrayExtra(EXTRA_CURRENTS) ?: FloatArray(0)
        val voltages = intent.getFloatArrayExtra(EXTRA_VOLTAGES) ?: FloatArray(0)
        samples = currents.indices.map {
            CircuitResistanceTester.Sample(currents[it], voltages.getOrElse(it) { 0f })
        }
        resistance = intent.getFloatExtra(EXTRA_RESISTANCE, 0f)
        vOc = intent.getFloatExtra(EXTRA_VOC, 0f)
        rSquared = intent.getFloatExtra(EXTRA_R2, 0f)
        fuse = intent.getFloatExtra(EXTRA_FUSE, 0f)
        maxCurrent = intent.getFloatExtra(EXTRA_MAXI, 0f)
        reliable = intent.getBooleanExtra(EXTRA_RELIABLE, false)

        bind()
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
            "Tested up to %.2f A (%.0f%% of %.1f A fuse)".format(
                maxCurrent, safePercent(), fuse
            ),
            "Points: ${samples.size}    Fit R²: %.4f".format(rSquared),
        ).joinToString("\n")

        if (!reliable) {
            binding.reliabilityText.visibility = android.view.View.VISIBLE
            binding.reliabilityText.setTextColor(Color.parseColor("#FFB300"))
            binding.reliabilityText.text = getString(R.string.rt_low_confidence)
        }

        binding.viChart.mode = ResistanceChartView.MODE_VI
        binding.viChart.setData(samples, resistance, vOc)
        binding.trendChart.mode = ResistanceChartView.MODE_TREND
        binding.trendChart.setData(samples, resistance, vOc)

        binding.dataTable.text = buildTable()
    }

    private fun safePercent(): Float = if (fuse > 0f) maxCurrent / fuse * 100f else 0f

    private fun buildTable(): String {
        val sb = StringBuilder()
        sb.append("Step   Current(A)   Voltage(V)   R@pt(Ω)\n")
        sb.append("--------------------------------------------\n")
        samples.forEachIndexed { idx, s ->
            val rPt = if (s.current > 1e-4f && vOc > 0f) (vOc - s.voltage) / s.current else 0f
            sb.append(
                "%-4d   %9.3f   %9.3f   %8.4f\n".format(idx + 1, s.current, s.voltage, rPt)
            )
        }
        return sb.toString().trimEnd()
    }

    private fun formatResistance(ohm: Float): String = when {
        ohm <= 0f -> "n/a"
        ohm < 1f -> "%.1f mΩ".format(ohm * 1000f)
        else -> "%.3f Ω".format(ohm)
    }

    // ---- Report rendering -------------------------------------------------
    private fun buildReportBitmap(): Bitmap {
        val w = 1080
        val margin = 48f
        val chartW = w - margin * 2
        val chartH = 620f
        val headerH = 320f
        val tableLines = samples.size + 3
        val tableH = 40f + tableLines * 34f
        val h = (headerH + chartH * 2 + tableH + margin * 3).toInt()

        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        c.drawColor(Color.WHITE)

        val title = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.parseColor("#102027"); textSize = 44f; isFakeBoldText = true
        }
        val body = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.parseColor("#37474F"); textSize = 30f
        }
        val big = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.parseColor("#00838F"); textSize = 68f; isFakeBoldText = true
        }
        val mono = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.parseColor("#102027"); textSize = 26f
            typeface = android.graphics.Typeface.MONOSPACE
        }

        var y = margin + 44f
        c.drawText("Circuit Resistance Test", margin, y, title)
        y += 40f
        val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault()).format(Date())
        c.drawText(stamp, margin, y, body)
        y += 66f
        c.drawText(formatResistance(resistance), margin, y, big)
        y += 44f
        c.drawText("Open-circuit voltage: %.3f V".format(vOc), margin, y, body); y += 38f
        c.drawText(
            "Tested up to %.2f A  (%.0f%% of %.1f A fuse)".format(maxCurrent, safePercent(), fuse),
            margin, y, body
        ); y += 38f
        c.drawText("Points: ${samples.size}    Fit R²: %.4f".format(rSquared), margin, y, body)
        y += 40f

        // Charts (light palette)
        binding.viChart.drawChart(c, margin, y, chartW, chartH, light = true)
        y += chartH + margin
        binding.trendChart.drawChart(c, margin, y, chartW, chartH, light = true)
        y += chartH + margin

        // Data table
        var ty = y + 30f
        for (line in buildTable().split("\n")) {
            c.drawText(line, margin, ty, mono)
            ty += 34f
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
        const val EXTRA_RESISTANCE = "resistance"
        const val EXTRA_VOC = "voc"
        const val EXTRA_R2 = "r2"
        const val EXTRA_FUSE = "fuse"
        const val EXTRA_MAXI = "maxi"
        const val EXTRA_RELIABLE = "reliable"

        fun start(from: AppCompatActivity, result: CircuitResistanceTester.ResistanceResult) {
            val i = Intent(from, ResultActivity::class.java).apply {
                putExtra(EXTRA_CURRENTS, FloatArray(result.samples.size) { result.samples[it].current })
                putExtra(EXTRA_VOLTAGES, FloatArray(result.samples.size) { result.samples[it].voltage })
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
