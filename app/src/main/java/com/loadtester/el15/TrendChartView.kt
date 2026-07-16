package com.loadtester.el15

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.abs
import kotlin.math.max

/**
 * Trend line for archived results: one metric (y) against test date (x),
 * drawn evenly spaced per test with date tick labels. Matches the blueprint
 * palette used by the other chart views.
 */
class TrendChartView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    data class Point(val timestampMs: Long, val value: Float)

    private var points: List<Point> = emptyList()
    private var unit: String = ""

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = android.graphics.Typeface.MONOSPACE
    }

    fun setData(pts: List<Point>, unitStr: String) {
        points = pts.sortedBy { it.timestampMs }
        unit = unitStr
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        val den = resources.displayMetrics.density
        fun dp(v: Float) = v * den
        val night = (resources.configuration.uiMode and
            android.content.res.Configuration.UI_MODE_NIGHT_MASK) ==
            android.content.res.Configuration.UI_MODE_NIGHT_YES
        val ink = Color.parseColor(if (night) "#E6EDF3" else "#102027")
        val gridC = Color.parseColor(if (night) "#2A3441" else "#DDDDDD")
        val axis = Color.parseColor(if (night) "#8B98A5" else "#455A64")
        val steel = Color.parseColor(if (night) "#7BA1C9" else "#3C6188")

        text.textSize = dp(10.5f)
        text.color = ink

        if (points.size < 2) {
            text.textAlign = Paint.Align.CENTER
            canvas.drawText(context.getString(R.string.trends_empty).split("\n").first(),
                width / 2f, height / 2f, text)
            return
        }

        val plot = RectF(dp(52f), dp(14f), width - dp(10f), height - dp(30f))
        var lo = points.minOf { it.value }
        var hi = points.maxOf { it.value }
        val pad = max((hi - lo) * 0.12f, abs(hi) * 0.01f + 1e-4f)
        lo -= pad; hi += pad

        fun x(idx: Int) = plot.left + idx.toFloat() / (points.size - 1) * plot.width()
        fun y(v: Float) = plot.bottom - (v - lo) / (hi - lo) * plot.height()

        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1f)
        paint.color = gridC
        for (k in 0..4) {
            val gy = plot.bottom - k / 4f * plot.height()
            canvas.drawLine(plot.left, gy, plot.right, gy, paint)
            text.textAlign = Paint.Align.RIGHT
            canvas.drawText(fmt(lo + k / 4f * (hi - lo)), plot.left - dp(5f), gy + dp(3.5f), text)
        }
        paint.color = axis
        paint.strokeWidth = dp(1.5f)
        canvas.drawLine(plot.left, plot.top, plot.left, plot.bottom, paint)
        canvas.drawLine(plot.left, plot.bottom, plot.right, plot.bottom, paint)

        // Date ticks: first, middle, last.
        val fmtDate = SimpleDateFormat("MM-dd", Locale.getDefault())
        text.textAlign = Paint.Align.CENTER
        for (idx in setOf(0, points.size / 2, points.size - 1)) {
            canvas.drawText(fmtDate.format(Date(points[idx].timestampMs)),
                x(idx), plot.bottom + dp(14f), text)
        }

        // Line + markers.
        paint.color = steel
        paint.strokeWidth = dp(2f)
        for (k in 1 until points.size) {
            canvas.drawLine(x(k - 1), y(points[k - 1].value), x(k), y(points[k].value), paint)
        }
        paint.style = Paint.Style.FILL
        for (k in points.indices) canvas.drawCircle(x(k), y(points[k].value), dp(3f), paint)

        // Latest value label.
        text.textAlign = Paint.Align.RIGHT
        text.color = steel
        canvas.drawText("latest ${fmt(points.last().value)}$unit", plot.right, plot.top + dp(4f), text)
    }

    private fun fmt(v: Float): String = when {
        abs(v) >= 100f -> String.format(Locale.US, "%.0f", v)
        abs(v) >= 10f -> String.format(Locale.US, "%.1f", v)
        else -> String.format(Locale.US, "%.2f", v)
    }
}
