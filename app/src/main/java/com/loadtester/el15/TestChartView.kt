package com.loadtester.el15

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View
import java.util.Locale
import kotlin.math.abs
import kotlin.math.max

/**
 * Configurable results chart for the test archive — five views over one sweep:
 *
 *  - [ChartConfig.VIEW_VI]      voltage vs current scatter + fitted R line
 *  - [ChartConfig.VIEW_TREND]   per-step multi-series, each normalized to its range
 *  - [ChartConfig.VIEW_ABS]     per-step selected series on a shared real-value axis
 *  - [ChartConfig.VIEW_R_I]     per-point resistance vs current (linearity check)
 *  - [ChartConfig.VIEW_P_I]     power vs current
 *
 * Style options (grid, markers, area fill, line weight) and the visible-series
 * set live in [ChartConfig]. The same renderer draws the on-screen (dark) chart
 * and the white report bitmap ([drawChart] with light=true).
 */
class TestChartView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    class ChartConfig(
        var view: Int = VIEW_VI,
        val series: MutableSet<Char> = mutableSetOf('V', 'I', 'P', 'T', 'F'),
        var grid: Boolean = true,
        var markers: Boolean = true,
        var fill: Boolean = false,
        var thick: Boolean = false,
    ) {
        fun encode(): String =
            "$view|${series.joinToString("")}|${if (grid) 1 else 0}${if (markers) 1 else 0}${if (fill) 1 else 0}${if (thick) 1 else 0}"

        companion object {
            const val VIEW_VI = 0
            const val VIEW_TREND = 1
            const val VIEW_ABS = 2
            const val VIEW_R_I = 3
            const val VIEW_P_I = 4

            fun decode(s: String?): ChartConfig {
                val c = ChartConfig()
                if (s == null) return c
                try {
                    val parts = s.split("|")
                    c.view = parts[0].toInt().coerceIn(0, 4)
                    c.series.clear()
                    parts[1].forEach { ch -> if (ch in "VIPTF") c.series.add(ch) }
                    if (c.series.isEmpty()) c.series.addAll(listOf('V', 'I'))
                    val flags = parts[2]
                    c.grid = flags.getOrNull(0) == '1'
                    c.markers = flags.getOrNull(1) == '1'
                    c.fill = flags.getOrNull(2) == '1'
                    c.thick = flags.getOrNull(3) == '1'
                } catch (e: Exception) {
                    return ChartConfig()
                }
                return c
            }
        }
    }

    var config: ChartConfig = ChartConfig()
        set(value) { field = value; invalidate() }

    private var samples: List<CircuitResistanceTester.Sample> = emptyList()
    private var resistance = 0f
    private var openCircuitV = 0f

    fun setData(samples: List<CircuitResistanceTester.Sample>, resistanceOhm: Float, openCircuitV: Float) {
        this.samples = samples
        this.resistance = resistanceOhm
        this.openCircuitV = openCircuitV
        invalidate()
    }

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG)
    private val fillPath = Path()

    /**
     * When set, dp()/sp() scale by this instead of device density/font scale —
     * used for report bitmaps whose pixel size is fixed regardless of device.
     */
    private var renderScale: Float? = null

    override fun onDraw(canvas: Canvas) {
        drawChart(canvas, 0f, 0f, width.toFloat(), height.toFloat(), light = false)
    }

    /** One trace over the sweep, in real units. */
    private class Trace(val key: Char, val name: String, val unit: String, val values: List<Float>)

    private fun palette(light: Boolean): Map<Char, Int> = mapOf(
        'V' to Color.parseColor(if (light) "#2E7D32" else "#4CAF50"),
        'I' to Color.parseColor(if (light) "#B26A00" else "#FFB300"),
        'P' to Color.parseColor(if (light) "#3C6188" else "#7BA1C9"),
        'T' to Color.parseColor(if (light) "#C62828" else "#EF5350"),
        'F' to Color.parseColor(if (light) "#6A1B9A" else "#AB47BC"),
    )

    private fun traces(): List<Trace> = listOf(
        Trace('V', "V", "V", samples.map { it.voltage }),
        Trace('I', "I", "A", samples.map { it.current }),
        Trace('P', "P", "W", samples.map { it.power }),
        Trace('T', "T", "°C", samples.map { it.temperature }),
        Trace('F', "Fan", "", samples.map { it.fanSpeed.toFloat() }),
    )

    private fun rAtPoint(s: CircuitResistanceTester.Sample): Float =
        if (s.current > 1e-4f && openCircuitV > 0f) (openCircuitV - s.voltage) / s.current else 0f

    fun drawChart(canvas: Canvas, left: Float, top: Float, w: Float, h: Float, light: Boolean) {
        drawChart(canvas, left, top, w, h, light, config)
    }

    /**
     * Report rendering can override the view config and the geometry scale
     * without touching screen state. [scale] pins dp/sp so a fixed-pixel
     * report bitmap renders identically on every device density/font scale.
     */
    fun drawChart(
        canvas: Canvas, left: Float, top: Float, w: Float, h: Float,
        light: Boolean, cfg: ChartConfig, scale: Float? = null,
    ) {
        renderScale = scale
        try {
            drawChartInner(canvas, left, top, w, h, light, cfg)
        } finally {
            renderScale = null
        }
    }

    private fun drawChartInner(canvas: Canvas, left: Float, top: Float, w: Float, h: Float, light: Boolean, cfg: ChartConfig) {
        val axis = Color.parseColor(if (light) "#455A64" else "#8B98A5")
        val label = Color.parseColor(if (light) "#102027" else "#E6EDF3")
        val grid = Color.parseColor(if (light) "#DDDDDD" else "#2A3441")

        val legendRoom = cfg.view == ChartConfig.VIEW_TREND || cfg.view == ChartConfig.VIEW_ABS
        val padL = dp(46f); val padR = dp(16f)
        val padT = dp(if (legendRoom) 62f else 18f); val padB = dp(34f)
        val plot = RectF(left + padL, top + padT, left + w - padR, top + h - padB)

        text.color = label
        text.textSize = sp(11f)
        text.typeface = android.graphics.Typeface.MONOSPACE

        if (samples.size < 2) {
            text.textAlign = Paint.Align.CENTER
            canvas.drawText("Not enough data", left + w / 2, top + h / 2, text)
            return
        }

        paint.color = axis; paint.strokeWidth = dp(1.5f); paint.style = Paint.Style.STROKE
        canvas.drawLine(plot.left, plot.top, plot.left, plot.bottom, paint)
        canvas.drawLine(plot.left, plot.bottom, plot.right, plot.bottom, paint)

        when (cfg.view) {
            ChartConfig.VIEW_VI -> drawXY(
                canvas, plot, cfg, grid, label,
                xs = samples.map { it.current }, ys = samples.map { it.voltage },
                color = palette(light).getValue('V'),
                xTitle = "Current (A)", yTitle = "Voltage (V)",
                fitLine = if (resistance > 0f) { i: Float -> openCircuitV - resistance * i } else null,
                fitColor = palette(light).getValue('P'),
                yIncludes = openCircuitV,
            )
            ChartConfig.VIEW_R_I -> {
                // Points where R@pt is undefined (no current / no Voc) would
                // plot as a 0 mΩ sentinel and wreck the axis — drop them.
                val valid = samples.filter { it.current > 1e-4f && openCircuitV > 0f }
                if (valid.size < 2) {
                    text.textAlign = Paint.Align.CENTER
                    canvas.drawText("Not enough valid points", plot.centerX(), plot.centerY(), text)
                } else {
                    drawXY(
                        canvas, plot, cfg, grid, label,
                        xs = valid.map { it.current }, ys = valid.map { rAtPoint(it) * 1000f },
                        color = palette(light).getValue('T'),
                        xTitle = "Current (A)", yTitle = "R at point (mΩ)",
                        fitLine = if (resistance > 0f) { _: Float -> resistance * 1000f } else null,
                        fitColor = palette(light).getValue('P'),
                        connect = true,
                    )
                }
            }
            ChartConfig.VIEW_P_I -> drawXY(
                canvas, plot, cfg, grid, label,
                xs = samples.map { it.current }, ys = samples.map { it.power },
                color = palette(light).getValue('P'),
                xTitle = "Current (A)", yTitle = "Power (W)",
                connect = true,
            )
            ChartConfig.VIEW_TREND -> drawTrend(canvas, plot, cfg, grid, label, light, normalized = true)
            ChartConfig.VIEW_ABS -> drawTrend(canvas, plot, cfg, grid, label, light, normalized = false)
        }
    }

    /** Scatter/line of ys against xs with real axes, optional analytic fit line. */
    private fun drawXY(
        canvas: Canvas, plot: RectF, cfg: ChartConfig, gridC: Int, label: Int,
        xs: List<Float>, ys: List<Float>, color: Int,
        xTitle: String, yTitle: String,
        fitLine: ((Float) -> Float)? = null, fitColor: Int = color,
        connect: Boolean = false, yIncludes: Float? = null,
    ) {
        val xMax = max(xs.max() * 1.05f, 1e-3f); val xMin = 0f
        var yHi = ys.max(); var yLo = ys.min()
        if (yIncludes != null) { yHi = max(yHi, yIncludes); yLo = minOf(yLo, yIncludes) }
        val yPad = max((yHi - yLo) * 0.15f, max(abs(yHi) * 0.02f, 0.02f))
        val yMax = yHi + yPad; val yMin = yLo - yPad

        fun x(v: Float) = plot.left + (v - xMin) / (xMax - xMin) * plot.width()
        fun y(v: Float) = plot.bottom - (v - yMin) / (yMax - yMin) * plot.height()

        drawGridAndTicks(canvas, plot, cfg, gridC, label, xMin, xMax, yMin, yMax)

        val lw = dp(if (cfg.thick) 3.2f else 2f)

        if (fitLine != null) {
            paint.color = fitColor; paint.style = Paint.Style.STROKE
            paint.strokeWidth = dp(if (cfg.thick) 2.6f else 1.8f)
            canvas.drawLine(x(xMin), y(fitLine(xMin)), x(xMax), y(fitLine(xMax)), paint)
        }

        if (cfg.fill && connect) {
            fillPath.reset()
            fillPath.moveTo(x(xs[0]), plot.bottom)
            for (k in xs.indices) fillPath.lineTo(x(xs[k]), y(ys[k]))
            fillPath.lineTo(x(xs.last()), plot.bottom)
            fillPath.close()
            paint.style = Paint.Style.FILL
            paint.color = withAlpha(color, 0.18f)
            canvas.drawPath(fillPath, paint)
        }

        if (connect) {
            paint.color = color; paint.style = Paint.Style.STROKE; paint.strokeWidth = lw
            for (k in 0 until xs.size - 1) {
                canvas.drawLine(x(xs[k]), y(ys[k]), x(xs[k + 1]), y(ys[k + 1]), paint)
            }
        }
        if (cfg.markers || !connect) {
            paint.color = color; paint.style = Paint.Style.FILL
            for (k in xs.indices) canvas.drawCircle(x(xs[k]), y(ys[k]), dp(if (cfg.thick) 4.5f else 3.5f), paint)
        }

        text.color = label; text.textAlign = Paint.Align.CENTER
        canvas.drawText(xTitle, plot.centerX(), plot.bottom + dp(26f), text)
        drawRotated(canvas, yTitle, plot.left - dp(34f), plot.centerY())
    }

    private fun drawTrend(
        canvas: Canvas, plot: RectF, cfg: ChartConfig, gridC: Int, label: Int,
        light: Boolean, normalized: Boolean,
    ) {
        val pal = palette(light)
        val active = traces().filter { it.key in cfg.series }
        if (active.isEmpty()) {
            text.textAlign = Paint.Align.CENTER
            canvas.drawText("No series selected", plot.centerX(), plot.centerY(), text)
            return
        }
        val n = samples.size
        fun x(idx: Int) = plot.left + idx.toFloat() / (n - 1) * plot.width()

        // Shared real axis for absolute view.
        var absLo = Float.MAX_VALUE; var absHi = -Float.MAX_VALUE
        if (!normalized) {
            for (t in active) { absLo = minOf(absLo, t.values.min()); absHi = max(absHi, t.values.max()) }
            val pad = max((absHi - absLo) * 0.1f, 0.02f)
            absLo -= pad; absHi += pad
        }

        if (cfg.grid) {
            paint.color = gridC; paint.style = Paint.Style.STROKE; paint.strokeWidth = dp(1f)
            for (k in 0..4) {
                val gy = plot.bottom - k / 4f * plot.height()
                canvas.drawLine(plot.left, gy, plot.right, gy, paint)
            }
        }
        if (!normalized) {
            text.textAlign = Paint.Align.RIGHT; text.color = label
            for (k in 0..4) {
                val v = absLo + k / 4f * (absHi - absLo)
                val gy = plot.bottom - k / 4f * plot.height()
                canvas.drawText(fmt(v), plot.left - dp(4f), gy + sp(4f), text)
            }
        }

        val lw = dp(if (cfg.thick) 3f else 2.2f)
        for (t in active) {
            val lo = t.values.min(); val hi = t.values.max()
            fun y(v: Float): Float = if (normalized) {
                val f = if (hi - lo > 1e-6f) (v - lo) / (hi - lo) else 0.5f
                plot.bottom - f * plot.height()
            } else {
                plot.bottom - (v - absLo) / (absHi - absLo) * plot.height()
            }
            val color = pal.getValue(t.key)
            if (cfg.fill) {
                fillPath.reset()
                fillPath.moveTo(x(0), plot.bottom)
                for (k in 0 until n) fillPath.lineTo(x(k), y(t.values[k]))
                fillPath.lineTo(x(n - 1), plot.bottom)
                fillPath.close()
                paint.style = Paint.Style.FILL; paint.color = withAlpha(color, 0.12f)
                canvas.drawPath(fillPath, paint)
            }
            paint.color = color; paint.style = Paint.Style.STROKE; paint.strokeWidth = lw
            for (k in 0 until n - 1) canvas.drawLine(x(k), y(t.values[k]), x(k + 1), y(t.values[k + 1]), paint)
            if (cfg.markers) {
                paint.style = Paint.Style.FILL
                for (k in 0 until n) canvas.drawCircle(x(k), y(t.values[k]), dp(2.6f), paint)
            }
        }

        text.color = label; text.textAlign = Paint.Align.CENTER
        canvas.drawText("Step", plot.centerX(), plot.bottom + dp(26f), text)
        if (normalized) drawRotated(canvas, "normalized", plot.left - dp(34f), plot.centerY())

        // Legend with real ranges, wrapping over up to two rows.
        text.textSize = sp(10f)
        var lx = plot.left
        var ly = plot.top - dp(34f)
        for (t in active) {
            val lo = t.values.min(); val hi = t.values.max()
            val range = if (hi - lo < 0.05f) fmt(hi) + t.unit else "${fmt(lo)}–${fmt(hi)}${t.unit}"
            val entry = "● ${t.name} $range"
            val need = text.measureText(entry) + dp(14f)
            if (lx + need > plot.right && lx > plot.left) { lx = plot.left; ly += dp(15f) }
            text.color = pal.getValue(t.key); text.textAlign = Paint.Align.LEFT
            canvas.drawText(entry, lx, ly, text)
            lx += need
        }
        text.textSize = sp(11f)
    }

    private fun drawGridAndTicks(
        canvas: Canvas, plot: RectF, cfg: ChartConfig, gridC: Int, label: Int,
        xMin: Float, xMax: Float, yMin: Float, yMax: Float,
    ) {
        paint.style = Paint.Style.STROKE; paint.strokeWidth = dp(1f)
        text.color = label
        for (k in 0..4) {
            val gx = plot.left + k / 4f * plot.width()
            val gy = plot.bottom - k / 4f * plot.height()
            if (cfg.grid) {
                paint.color = gridC
                canvas.drawLine(gx, plot.top, gx, plot.bottom, paint)
                canvas.drawLine(plot.left, gy, plot.right, gy, paint)
            }
            text.textAlign = Paint.Align.CENTER
            canvas.drawText(fmt(xMin + k / 4f * (xMax - xMin)), gx, plot.bottom + dp(14f), text)
            text.textAlign = Paint.Align.RIGHT
            canvas.drawText(fmt(yMin + k / 4f * (yMax - yMin)), plot.left - dp(4f), gy + sp(4f), text)
        }
    }

    private fun fmt(v: Float): String = when {
        abs(v) >= 100f -> String.format(Locale.US, "%.0f", v)
        abs(v) >= 10f -> String.format(Locale.US, "%.1f", v)
        else -> String.format(Locale.US, "%.2f", v)
    }

    private fun drawRotated(canvas: Canvas, s: String, cx: Float, cy: Float) {
        canvas.save()
        canvas.rotate(-90f, cx, cy)
        text.textAlign = Paint.Align.CENTER
        canvas.drawText(s, cx, cy, text)
        canvas.restore()
    }

    private fun withAlpha(color: Int, a: Float): Int =
        Color.argb((a * 255).toInt(), Color.red(color), Color.green(color), Color.blue(color))

    private fun dp(v: Float) = v * (renderScale ?: resources.displayMetrics.density)
    private fun sp(v: Float) = v * (renderScale ?: resources.displayMetrics.scaledDensity)
}
