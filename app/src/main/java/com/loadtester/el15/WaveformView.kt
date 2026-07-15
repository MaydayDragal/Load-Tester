package com.loadtester.el15

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View
import kotlin.math.max

/**
 * Live scrolling waveform of voltage and current. Keeps a capped display buffer
 * plus an optional full-resolution recording buffer for CSV export, and exposes
 * min/max/avg over the visible window. Pause freezes the display without losing
 * the connection.
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
    private val record = ArrayList<WPoint>()
    private val maxDisplay = 600

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG)

    fun add(v: Float, i: Float, p: Float, temp: Float, fan: Int, tMs: Long) {
        if (recording) {
            record.add(WPoint(tMs, v, i, p, temp, fan))
            if (record.size > 200_000) record.removeAt(0) // safety cap
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

    /** min/max/avg over the visible window, as a compact one-liner. */
    fun statsText(): String {
        if (display.isEmpty()) return "—"
        val vs = display.map { it.v }; val is_ = display.map { it.i }; val ps = display.map { it.p }
        fun mma(x: List<Float>, u: String, dp: Int) =
            "%.${dp}f/%.${dp}f/%.${dp}f$u".format(x.min(), x.average().toFloat(), x.max())
        return "V ${mma(vs, "", 2)}   I ${mma(is_, "", 2)}   P ${mma(ps, "", 1)}  (min/avg/max)"
    }

    override fun onDraw(canvas: Canvas) {
        val bg = themeColor(com.google.android.material.R.attr.colorSurface, Color.BLACK)
        val gridC = blend(bg, themeColor(com.google.android.material.R.attr.colorOnSurface, Color.GRAY), 0.12f)
        val vC = ContextColor.get(context, R.color.value_green)
        val iC = ContextColor.get(context, R.color.value_amber)

        val padL = dp(6f); val padR = dp(6f); val padT = dp(6f); val padB = dp(6f)
        val plot = RectF(padL, padT, width - padR, height - padB)

        // Grid
        paint.style = Paint.Style.STROKE; paint.strokeWidth = dp(1f); paint.color = gridC
        for (k in 0..4) {
            val gy = plot.top + k / 4f * plot.height()
            canvas.drawLine(plot.left, gy, plot.right, gy, paint)
        }

        if (display.size < 2) {
            text.color = gridC; text.textSize = sp(12f); text.textAlign = Paint.Align.CENTER
            canvas.drawText("Waiting for data…", plot.centerX(), plot.centerY(), text)
            return
        }

        drawSeries(canvas, plot, display.map { it.v }, vC)
        drawSeries(canvas, plot, display.map { it.i }, iC)
    }

    private fun drawSeries(canvas: Canvas, plot: RectF, values: List<Float>, color: Int) {
        val lo = values.min(); val hi = values.max()
        val span = max(hi - lo, 1e-4f)
        val n = values.size
        fun x(k: Int) = plot.left + k.toFloat() / (n - 1) * plot.width()
        fun y(v: Float) = plot.bottom - (v - lo) / span * plot.height() * 0.9f - plot.height() * 0.05f
        paint.style = Paint.Style.STROKE; paint.strokeWidth = dp(2f); paint.color = color
        var px = x(0); var py = y(values[0])
        for (k in 1 until n) {
            val cx = x(k); val cy = y(values[k])
            canvas.drawLine(px, py, cx, cy, paint)
            px = cx; py = cy
        }
    }

    private fun themeColor(attr: Int, fallback: Int): Int {
        val tv = android.util.TypedValue()
        return if (context.theme.resolveAttribute(attr, tv, true)) tv.data else fallback
    }

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
}

/** Small helper to read a color resource across API levels. */
object ContextColor {
    fun get(context: Context, res: Int): Int =
        androidx.core.content.ContextCompat.getColor(context, res)
}
