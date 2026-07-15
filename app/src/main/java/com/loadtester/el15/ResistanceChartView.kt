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
 * Lightweight Canvas chart for circuit-resistance results. No external deps.
 *
 * Two modes:
 *  - [MODE_VI]: voltage (Y) against current (X) with the fitted resistance line.
 *  - [MODE_TREND]: voltage and current against step index (dual scaled axes).
 *
 * The drawing routine is shared with a light "export" palette so the same chart
 * can be rendered onto a white report bitmap for printing/sharing.
 */
class ResistanceChartView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    companion object {
        const val MODE_VI = 0
        const val MODE_TREND = 1
    }

    var mode: Int = MODE_VI
        set(value) { field = value; invalidate() }

    private var samples: List<CircuitResistanceTester.Sample> = emptyList()
    private var resistance: Float = 0f
    private var openCircuitV: Float = 0f

    fun setData(
        samples: List<CircuitResistanceTester.Sample>,
        resistanceOhm: Float,
        openCircuitV: Float,
    ) {
        this.samples = samples
        this.resistance = resistanceOhm
        this.openCircuitV = openCircuitV
        invalidate()
    }

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = spToPx(12f) }

    override fun onDraw(canvas: Canvas) {
        drawChart(canvas, 0f, 0f, width.toFloat(), height.toFloat(), light = false)
    }

    /** Draw the chart into an arbitrary region; used by onDraw and report export. */
    fun drawChart(canvas: Canvas, left: Float, top: Float, w: Float, h: Float, light: Boolean) {
        val axis = if (light) Color.parseColor("#455A64") else Color.parseColor("#8B98A5")
        val label = if (light) Color.parseColor("#102027") else Color.parseColor("#E6EDF3")
        val grid = if (light) Color.parseColor("#DDDDDD") else Color.parseColor("#2A3441")
        val vColor = Color.parseColor("#4CAF50")
        val iColor = Color.parseColor("#FFB300")
        val fit = Color.parseColor("#00ACC1")

        val padL = dpToPx(44f); val padR = dpToPx(16f)
        // Trend mode needs headroom for a multi-series legend.
        val padT = dpToPx(if (mode == MODE_TREND) 62f else 16f); val padB = dpToPx(34f)
        val plot = RectF(left + padL, top + padT, left + w - padR, top + h - padB)

        text.color = label
        text.textSize = spToPx(11f)

        if (samples.size < 2) {
            text.textAlign = Paint.Align.CENTER
            canvas.drawText("Not enough data", left + w / 2, top + h / 2, text)
            return
        }

        // Axes
        paint.color = axis; paint.strokeWidth = dpToPx(1.5f); paint.style = Paint.Style.STROKE
        canvas.drawLine(plot.left, plot.top, plot.left, plot.bottom, paint)
        canvas.drawLine(plot.left, plot.bottom, plot.right, plot.bottom, paint)

        if (mode == MODE_VI) drawVi(canvas, plot, grid, vColor, fit, label)
        else drawTrend(canvas, plot, grid, label)
    }

    private fun drawVi(canvas: Canvas, plot: RectF, grid: Int, pointColor: Int, fit: Int, label: Int) {
        val iMax = max(samples.maxOf { it.current } * 1.05f, 1e-3f)
        val iMin = 0f
        val vHi = max(samples.maxOf { it.voltage }, openCircuitV)
        val vLo = samples.minOf { it.voltage }
        val vPad = max((vHi - vLo) * 0.15f, 0.02f)
        val vMax = vHi + vPad; val vMin = vLo - vPad

        fun x(i: Float) = plot.left + (i - iMin) / (iMax - iMin) * plot.width()
        fun y(v: Float) = plot.bottom - (v - vMin) / (vMax - vMin) * plot.height()

        drawGridAndTicks(canvas, plot, grid, label, iMin, iMax, vMin, vMax)

        // Fitted resistance line: V = Voc - R·I
        if (resistance > 0f) {
            paint.color = fit; paint.style = Paint.Style.STROKE; paint.strokeWidth = dpToPx(2f)
            val v0 = openCircuitV
            val v1 = openCircuitV - resistance * iMax
            canvas.drawLine(x(iMin), y(v0), x(iMax), y(v1), paint)
        }

        // Measured points
        paint.color = pointColor; paint.style = Paint.Style.FILL
        for (s in samples) canvas.drawCircle(x(s.current), y(s.voltage), dpToPx(4f), paint)

        // Axis titles
        text.color = label; text.textAlign = Paint.Align.CENTER
        canvas.drawText("Current (A)", plot.centerX(), plot.bottom + dpToPx(26f), text)
        drawRotated(canvas, "Voltage (V)", plot.left - dpToPx(32f), plot.centerY())
    }

    /** One trace on the normalized trend plot. */
    private class Trace(val name: String, val unit: String, val color: Int, val values: List<Float>)

    private fun drawTrend(canvas: Canvas, plot: RectF, grid: Int, label: Int) {
        val n = samples.size
        val traces = listOf(
            Trace("V", "V", Color.parseColor("#4CAF50"), samples.map { it.voltage }),
            Trace("I", "A", Color.parseColor("#FFB300"), samples.map { it.current }),
            Trace("P", "W", Color.parseColor("#00ACC1"), samples.map { it.power }),
            Trace("T", "°C", Color.parseColor("#EF5350"), samples.map { it.temperature }),
            Trace("Fan", "", Color.parseColor("#AB47BC"), samples.map { it.fanSpeed.toFloat() }),
        )

        fun x(idx: Int) = plot.left + idx.toFloat() / (n - 1) * plot.width()
        // Each trace is normalized to its own min..max so they share the plot;
        // real values are shown in the legend and the data table. A flat trace
        // sits on the mid-line.
        fun yNorm(v: Float, lo: Float, hi: Float): Float {
            val f = if (hi - lo > 1e-6f) (v - lo) / (hi - lo) else 0.5f
            return plot.bottom - f * plot.height()
        }

        // Horizontal grid
        paint.color = grid; paint.style = Paint.Style.STROKE; paint.strokeWidth = dpToPx(1f)
        for (k in 0..4) {
            val gy = plot.bottom - k / 4f * plot.height()
            canvas.drawLine(plot.left, gy, plot.right, gy, paint)
        }

        // Series
        for (t in traces) {
            val lo = t.values.min(); val hi = t.values.max()
            paint.color = t.color
            paint.style = Paint.Style.STROKE; paint.strokeWidth = dpToPx(2.2f)
            for (k in 0 until n - 1) {
                canvas.drawLine(
                    x(k), yNorm(t.values[k], lo, hi),
                    x(k + 1), yNorm(t.values[k + 1], lo, hi), paint
                )
            }
            paint.style = Paint.Style.FILL
            for (k in 0 until n) canvas.drawCircle(x(k), yNorm(t.values[k], lo, hi), dpToPx(2.5f), paint)
        }

        // Axis titles
        text.color = label
        text.textAlign = Paint.Align.CENTER
        canvas.drawText("Step", plot.centerX(), plot.bottom + dpToPx(26f), text)
        drawRotated(canvas, "normalized", plot.left - dpToPx(32f), plot.centerY())

        // Legend with each series' real range, flowed across up to two rows.
        text.textSize = spToPx(10f)
        var lx = plot.left
        var ly = plot.top - dpToPx(34f)
        val rowGap = dpToPx(15f)
        for (t in traces) {
            val lo = t.values.min(); val hi = t.values.max()
            val labelText = "● ${t.name} ${fmtRange(lo, hi, t.unit)}"
            val wNeeded = text.measureText(labelText) + dpToPx(14f)
            if (lx + wNeeded > plot.right && lx > plot.left) {
                lx = plot.left; ly += rowGap
            }
            text.color = t.color; text.textAlign = Paint.Align.LEFT
            canvas.drawText(labelText, lx, ly, text)
            lx += wNeeded
        }
        text.textSize = spToPx(11f)
    }

    private fun fmtRange(lo: Float, hi: Float, unit: String): String {
        return if (hi - lo < 0.05f) "%.2f%s".format(hi, unit)
        else "%.2f–%.2f%s".format(lo, hi, unit)
    }

    private fun drawGridAndTicks(
        canvas: Canvas, plot: RectF, grid: Int, label: Int,
        xMin: Float, xMax: Float, yMin: Float, yMax: Float,
    ) {
        paint.color = grid; paint.style = Paint.Style.STROKE; paint.strokeWidth = dpToPx(1f)
        text.color = label
        for (k in 0..4) {
            val gx = plot.left + k / 4f * plot.width()
            val gy = plot.bottom - k / 4f * plot.height()
            canvas.drawLine(gx, plot.top, gx, plot.bottom, paint)
            canvas.drawLine(plot.left, gy, plot.right, gy, paint)

            text.textAlign = Paint.Align.CENTER
            val xv = xMin + k / 4f * (xMax - xMin)
            canvas.drawText("%.1f".format(xv), gx, plot.bottom + dpToPx(14f), text)

            text.textAlign = Paint.Align.RIGHT
            val yv = yMin + k / 4f * (yMax - yMin)
            canvas.drawText("%.2f".format(yv), plot.left - dpToPx(4f), gy + spToPx(4f), text)
        }
    }

    private fun drawRotated(canvas: Canvas, s: String, cx: Float, cy: Float) {
        canvas.save()
        canvas.rotate(-90f, cx, cy)
        text.textAlign = Paint.Align.CENTER
        canvas.drawText(s, cx, cy, text)
        canvas.restore()
    }

    private fun dpToPx(dp: Float) = dp * resources.displayMetrics.density
    private fun spToPx(sp: Float) = sp * resources.displayMetrics.scaledDensity
}
