package com.loadtester.el15

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.util.TypedValue
import android.view.View
import java.util.Locale
import kotlin.math.max

/**
 * Live scrolling waveform of voltage and current. Keeps a capped display buffer
 * plus an optional full-resolution recording buffer for CSV export, and exposes
 * min/max/avg over the visible window. Pause freezes the display without losing
 * the connection.
 *
 * Drawing avoids per-frame allocation: samples are copied into flat float
 * arrays once per frame and both series render from them.
 */
class WaveformView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    data class WPoint(
        val tMs: Long,
        val v: Float,
        val i: Float,
        val p: Float,
        val temp: Float,
        val fan: Int,
    )

    var paused = false
    var recording = false
        private set

    private val display = ArrayDeque<WPoint>()
    private val record = ArrayDeque<WPoint>()
    private val maxDisplay = 600

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG)

    // Reused per-frame buffers (no allocation in onDraw once warmed).
    private var xs = FloatArray(maxDisplay)
    private var vsBuf = FloatArray(maxDisplay)
    private var isBuf = FloatArray(maxDisplay)
    private val tv = TypedValue()

    fun add(v: Float, i: Float, p: Float, temp: Float, fan: Int, tMs: Long) {
        if (recording) {
            record.addLast(WPoint(tMs, v, i, p, temp, fan))
            if (record.size > MAX_RECORD) record.removeFirst() // O(1) ring behaviour
        }
        if (paused) return
        display.addLast(WPoint(tMs, v, i, p, temp, fan))
        while (display.size > maxDisplay) display.removeFirst()
        invalidate()
    }

    fun startRecording() { record.clear(); recording = true }
    fun stopRecording() { recording = false }
    fun recordedCount(): Int = record.size

    fun clear() {
        display.clear(); record.clear(); invalidate()
    }

    /** Rows for CSV export: the full recording if any, else the visible window. */
    fun exportRows(): List<WPoint> = if (record.isNotEmpty()) record.toList() else display.toList()

    /** min/avg/max over the visible window, as a compact one-liner. */
    fun statsText(): String {
        if (display.isEmpty()) return "—"
        var vMin = Float.MAX_VALUE; var vMax = -Float.MAX_VALUE; var vSum = 0.0
        var iMin = Float.MAX_VALUE; var iMax = -Float.MAX_VALUE; var iSum = 0.0
        var pMin = Float.MAX_VALUE; var pMax = -Float.MAX_VALUE; var pSum = 0.0
        for (s in display) {
            if (s.v < vMin) vMin = s.v; if (s.v > vMax) vMax = s.v; vSum += s.v
            if (s.i < iMin) iMin = s.i; if (s.i > iMax) iMax = s.i; iSum += s.i
            if (s.p < pMin) pMin = s.p; if (s.p > pMax) pMax = s.p; pSum += s.p
        }
        val n = display.size
        return String.format(
            Locale.US,
            "V %.2f/%.2f/%.2f   I %.2f/%.2f/%.2f   P %.1f/%.1f/%.1f  (min/avg/max)",
            vMin, vSum / n, vMax, iMin, iSum / n, iMax, pMin, pSum / n, pMax,
        )
    }

    override fun onDraw(canvas: Canvas) {
        val bg = themeColor(com.google.android.material.R.attr.colorSurface, Color.BLACK)
        val gridC = blend(bg, themeColor(com.google.android.material.R.attr.colorOnSurface, Color.GRAY), 0.12f)
        val vC = ContextColor.get(context, R.color.value_green)
        val iC = ContextColor.get(context, R.color.value_amber)

        val padL = dp(6f); val padR = dp(6f); val padT = dp(6f); val padB = dp(6f)
        val plotL = padL; val plotT = padT
        val plotR = width - padR; val plotB = height - padB
        val plotW = plotR - plotL; val plotH = plotB - plotT

        // Grid
        paint.style = Paint.Style.STROKE; paint.strokeWidth = dp(1f); paint.color = gridC
        for (k in 0..4) {
            val gy = plotT + k / 4f * plotH
            canvas.drawLine(plotL, gy, plotR, gy, paint)
        }

        val n = display.size
        if (n < 2) {
            text.color = gridC; text.textSize = sp(12f); text.textAlign = Paint.Align.CENTER
            canvas.drawText("Waiting for data…", plotL + plotW / 2, plotT + plotH / 2, text)
            return
        }

        // Snapshot into flat reusable buffers (single pass).
        if (xs.size < n) { xs = FloatArray(n); vsBuf = FloatArray(n); isBuf = FloatArray(n) }
        var k = 0
        for (s in display) {
            xs[k] = plotL + k.toFloat() / (n - 1) * plotW
            vsBuf[k] = s.v; isBuf[k] = s.i
            k++
        }
        drawSeries(canvas, n, vsBuf, vC, plotT, plotH, plotB)
        drawSeries(canvas, n, isBuf, iC, plotT, plotH, plotB)
    }

    private fun drawSeries(canvas: Canvas, n: Int, values: FloatArray, color: Int,
                           plotT: Float, plotH: Float, plotB: Float) {
        var lo = Float.MAX_VALUE; var hi = -Float.MAX_VALUE
        for (k in 0 until n) { if (values[k] < lo) lo = values[k]; if (values[k] > hi) hi = values[k] }
        val span = max(hi - lo, 1e-4f)
        fun y(v: Float) = plotB - (v - lo) / span * plotH * 0.9f - plotH * 0.05f
        paint.style = Paint.Style.STROKE; paint.strokeWidth = dp(2f); paint.color = color
        var px = xs[0]; var py = y(values[0])
        for (k in 1 until n) {
            val cx = xs[k]; val cy = y(values[k])
            canvas.drawLine(px, py, cx, cy, paint)
            px = cx; py = cy
        }
    }

    private fun themeColor(attr: Int, fallback: Int): Int =
        if (context.theme.resolveAttribute(attr, tv, true)) tv.data else fallback

    private fun blend(a: Int, b: Int, t: Float): Int {
        fun c(s: Int, e: Int) = (s + (e - s) * t).toInt()
        return Color.rgb(
            c(Color.red(a), Color.red(b)),
            c(Color.green(a), Color.green(b)),
            c(Color.blue(a), Color.blue(b)),
        )
    }

    private fun dp(v: Float) = v * resources.displayMetrics.density
    private fun sp(v: Float) = v * resources.displayMetrics.scaledDensity

    companion object {
        private const val MAX_RECORD = 200_000
    }
}

/** Small helper to read a color resource across API levels. */
object ContextColor {
    fun get(context: Context, res: Int): Int =
        androidx.core.content.ContextCompat.getColor(context, res)
}
