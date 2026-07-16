package com.loadtester.el15

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import java.util.Locale
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/**
 * Interactive time-series chart for session records and trend views:
 * pinch-zoom + drag-pan on the time axis, and a tap/drag cursor that pins the
 * nearest sample and shows its values. Series are selectable; V is drawn on
 * the left axis, everything else normalized unless it's the only series.
 * Also renders into report canvases via [drawInto] (light palette + fixed
 * scale, no interaction).
 */
class TimeSeriesChartView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    data class Series(val key: Char, val name: String, val unit: String, val color: Int)

    private var points: List<SessionRecord.TimePoint> = emptyList()
    val visible = linkedSetOf('V', 'I')

    // View window over [0,1] of the full time span.
    private var winStart = 0f
    private var winEnd = 1f
    private var cursorT: Long? = null

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = android.graphics.Typeface.MONOSPACE
    }

    fun setData(pts: List<SessionRecord.TimePoint>) {
        points = pts
        winStart = 0f; winEnd = 1f; cursorT = null
        invalidate()
    }

    fun toggleSeries(key: Char) {
        if (key in visible) { if (visible.size > 1) visible.remove(key) } else visible.add(key)
        invalidate()
    }

    fun resetView() { winStart = 0f; winEnd = 1f; cursorT = null; invalidate() }

    private fun seriesDefs(light: Boolean): List<Series> = listOf(
        Series('V', "V", "V", Color.parseColor(if (light) "#2E7D32" else "#4CAF50")),
        Series('I', "I", "A", Color.parseColor(if (light) "#B26A00" else "#FFB300")),
        Series('P', "P", "W", Color.parseColor(if (light) "#3C6188" else "#7BA1C9")),
        Series('T', "T", "°C", Color.parseColor(if (light) "#C62828" else "#EF5350")),
    )

    private fun valueOf(p: SessionRecord.TimePoint, key: Char): Float = when (key) {
        'V' -> p.v; 'I' -> p.i; 'P' -> p.p; else -> p.temp
    }

    // ---- Interaction --------------------------------------------------------
    private val scaleDetector = ScaleGestureDetector(context,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(d: ScaleGestureDetector): Boolean {
                val span = winEnd - winStart
                val focus = winStart + span * ((d.focusX - plotL) / max(1f, plotR - plotL)).coerceIn(0f, 1f)
                val newSpan = (span / d.scaleFactor).coerceIn(0.01f, 1f)
                winStart = (focus - (focus - winStart) * newSpan / span).coerceIn(0f, 1f - newSpan)
                winEnd = winStart + newSpan
                invalidate()
                return true
            }
        })

    private var lastDragX = 0f
    private var dragging = false
    private var plotL = 0f; private var plotR = 0f

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        scaleDetector.onTouchEvent(event)
        if (scaleDetector.isInProgress) { dragging = false; return true }
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastDragX = event.x; dragging = true
                updateCursor(event.x)
                parent?.requestDisallowInterceptTouchEvent(true)
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = event.x - lastDragX
                if (winEnd - winStart < 0.999f && abs(dx) > 6f && dragging) {
                    // Pan when zoomed; otherwise the drag moves the cursor.
                    val span = winEnd - winStart
                    val shift = -dx / max(1f, plotR - plotL) * span
                    winStart = (winStart + shift).coerceIn(0f, 1f - span)
                    winEnd = winStart + span
                    lastDragX = event.x
                    invalidate()
                } else {
                    updateCursor(event.x)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                dragging = false
                parent?.requestDisallowInterceptTouchEvent(false)
            }
        }
        return true
    }

    private fun updateCursor(x: Float) {
        if (points.isEmpty() || plotR <= plotL) return
        val span = points.last().tMs - points.first().tMs
        if (span <= 0) return
        val frac = ((x - plotL) / (plotR - plotL)).coerceIn(0f, 1f)
        val tWin0 = points.first().tMs + (span * winStart).toLong()
        val tWin1 = points.first().tMs + (span * winEnd).toLong()
        cursorT = tWin0 + ((tWin1 - tWin0) * frac).toLong()
        invalidate()
    }

    // ---- Rendering -----------------------------------------------------------
    override fun onDraw(canvas: Canvas) {
        drawInto(canvas, 0f, 0f, width.toFloat(), height.toFloat(), light = false, scale = null,
            interactive = true)
    }

    fun drawInto(
        canvas: Canvas, left: Float, top: Float, w: Float, h: Float,
        light: Boolean, scale: Float?, interactive: Boolean = false,
    ) {
        val den = scale ?: resources.displayMetrics.density
        fun dp(v: Float) = v * den
        val ink = Color.parseColor(if (light) "#102027" else "#E6EDF3")
        val gridC = Color.parseColor(if (light) "#DDDDDD" else "#2A3441")
        val axis = Color.parseColor(if (light) "#455A64" else "#8B98A5")

        val padL = dp(46f); val padR = dp(10f); val padT = dp(26f); val padB = dp(30f)
        val plot = RectF(left + padL, top + padT, left + w - padR, top + h - padB)
        if (interactive) { plotL = plot.left; plotR = plot.right }

        text.textSize = dp(10.5f); text.color = ink

        if (points.size < 2) {
            text.textAlign = Paint.Align.CENTER
            canvas.drawText("No data", left + w / 2, top + h / 2, text)
            return
        }

        val t0 = points.first().tMs; val t1 = points.last().tMs
        val span = max(1L, t1 - t0)
        val wt0 = t0 + (span * (if (interactive) winStart else 0f)).toLong()
        val wt1 = t0 + (span * (if (interactive) winEnd else 1f)).toLong()
        val visiblePts = points.filter { it.tMs in wt0..wt1 }.ifEmpty { points }

        fun x(t: Long) = plot.left + (t - wt0).toFloat() / max(1L, wt1 - wt0) * plot.width()

        // Grid + time ticks
        paint.style = Paint.Style.STROKE; paint.strokeWidth = dp(1f); paint.color = gridC
        for (k in 0..4) {
            val gx = plot.left + k / 4f * plot.width()
            val gy = plot.bottom - k / 4f * plot.height()
            canvas.drawLine(gx, plot.top, gx, plot.bottom, paint)
            canvas.drawLine(plot.left, gy, plot.right, gy, paint)
            val tick = (wt0 + (wt1 - wt0) * k / 4) / 1000
            text.textAlign = Paint.Align.CENTER
            canvas.drawText(SessionRecord.fmtDuration(tick), gx, plot.bottom + dp(13f), text)
        }
        paint.color = axis; paint.strokeWidth = dp(1.5f)
        canvas.drawLine(plot.left, plot.top, plot.left, plot.bottom, paint)
        canvas.drawLine(plot.left, plot.bottom, plot.right, plot.bottom, paint)

        val defs = seriesDefs(light).filter { it.key in visible }
        // Left axis follows the first visible series; others normalized to own range.
        val primary = defs.firstOrNull() ?: return
        var legendX = plot.left
        for ((idx, def) in defs.withIndex()) {
            val vals = visiblePts.map { valueOf(it, def.key) }
            var lo = vals.min(); var hi = vals.max()
            val pad = max((hi - lo) * 0.08f, 0.01f); lo -= pad; hi += pad
            fun y(v: Float) = plot.bottom - (v - lo) / (hi - lo) * plot.height()

            if (idx == 0) {
                text.textAlign = Paint.Align.RIGHT; text.color = def.color
                for (k in 0..4) {
                    val v = lo + k / 4f * (hi - lo)
                    canvas.drawText(fmt(v), plot.left - dp(4f), plot.bottom - k / 4f * plot.height() + dp(3.5f), text)
                }
            }
            paint.style = Paint.Style.STROKE; paint.strokeWidth = dp(2f); paint.color = def.color
            var px = x(visiblePts[0].tMs); var py = y(vals[0])
            for (k in 1 until visiblePts.size) {
                val cx = x(visiblePts[k].tMs); val cy = y(vals[k])
                canvas.drawLine(px, py, cx, cy, paint)
                px = cx; py = cy
            }
            // Legend
            text.textAlign = Paint.Align.LEFT; text.color = def.color
            val label = "● ${def.name} ${fmt(vals.last())}${def.unit}"
            canvas.drawText(label, legendX, plot.top - dp(8f), text)
            legendX += text.measureText(label) + dp(12f)
        }

        // Cursor
        if (interactive) cursorT?.let { ct ->
            val nearest = visiblePts.minByOrNull { abs(it.tMs - ct) } ?: return@let
            val cx = x(nearest.tMs)
            paint.color = ink; paint.strokeWidth = dp(1f)
            canvas.drawLine(cx, plot.top, cx, plot.bottom, paint)
            text.textAlign = if (cx > plot.centerX()) Paint.Align.RIGHT else Paint.Align.LEFT
            text.color = ink
            val tx = if (cx > plot.centerX()) cx - dp(6f) else cx + dp(6f)
            var ty = plot.top + dp(14f)
            canvas.drawText("t ${SessionRecord.fmtDuration(nearest.tMs / 1000)}", tx, ty, text)
            for (def in defs) {
                ty += dp(13f)
                canvas.drawText("${def.name} ${fmt(valueOf(nearest, def.key))}${def.unit}", tx, ty, text)
            }
        }
    }

    private fun fmt(v: Float): String = when {
        abs(v) >= 100f -> String.format(Locale.US, "%.0f", v)
        abs(v) >= 10f -> String.format(Locale.US, "%.1f", v)
        else -> String.format(Locale.US, "%.2f", v)
    }
}
