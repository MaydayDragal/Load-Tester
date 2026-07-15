package com.loadtester.el15

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.util.AttributeSet
import android.util.TypedValue
import android.view.View

/**
 * Semicircular instrument gauge: a track arc + a coloured value arc, with the
 * live value, unit and a "LABEL · 0–maxX" caption rendered inside the arc.
 * Mirrors the prototype's 180×96 arc (M18 90 A72 72 0 0 1 162 90).
 */
class ArcGaugeView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    var arcColor = Color.parseColor("#4CAF50")
    var unit = "V"
    var caption = ""
    var connected = true
    private var value = 0f
    private var maxValue = 15f
    private var decimals = 2

    private val arc = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val num = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.MONOSPACE; isFakeBoldText = true; textAlign = Paint.Align.CENTER
    }
    private val small = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.MONOSPACE; textAlign = Paint.Align.LEFT
    }
    private val cap = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.MONOSPACE; isFakeBoldText = true; textAlign = Paint.Align.CENTER
    }

    fun set(value: Float, maxValue: Float, decimals: Int, caption: String, connected: Boolean) {
        this.value = value; this.maxValue = maxValue; this.decimals = decimals
        this.caption = caption; this.connected = connected
        invalidate()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        // Honour the MeasureSpec contract: resolve the width against the spec,
        // then derive the 180:96 aspect height and resolve that too.
        val w = resolveSize(suggestedMinimumWidth.coerceAtLeast(
            MeasureSpec.getSize(widthMeasureSpec)), widthMeasureSpec)
        val desiredH = (w * 96f / 180f).toInt()
        setMeasuredDimension(w, resolveSize(desiredH, heightMeasureSpec))
    }

    override fun onDraw(canvas: Canvas) {
        val scale = width / 180f
        val cx = 90f * scale
        val cy = 90f * scale
        val r = 72f * scale
        arc.strokeWidth = 9f * scale
        arc.strokeCap = Paint.Cap.BUTT
        val oval = RectF(cx - r, cy - r, cx + r, cy + r)

        val track = withAlpha(attrColor(com.google.android.material.R.attr.colorOnSurface, Color.GRAY), 0.12f)
        arc.color = track
        canvas.drawArc(oval, 180f, 180f, false, arc)

        if (connected) {
            val frac = (value / maxValue).coerceIn(0f, 1f)
            arc.color = arcColor
            canvas.drawArc(oval, 180f, 180f * frac, false, arc)
        }

        val ink = attrColor(com.google.android.material.R.attr.colorOnSurface, Color.WHITE)
        val muted = withAlpha(ink, 0.5f)
        num.color = if (connected) ink else withAlpha(ink, 0.35f)
        num.textSize = 26f * scale
        val valueStr = if (connected) "%.${decimals}f".format(value) else "—"
        // Draw number centred, with the small unit appended to the right.
        small.textSize = 13f * scale
        small.color = muted
        val numW = num.measureText(valueStr)
        val unitW = small.measureText(unit)
        val totalW = numW + 3f * scale + unitW
        val startX = cx - totalW / 2f
        val baseY = cy - 6f * scale
        num.textAlign = Paint.Align.LEFT
        canvas.drawText(valueStr, startX, baseY, num)
        canvas.drawText(unit, startX + numW + 3f * scale, baseY, small)

        cap.textSize = 9.5f * scale
        cap.color = muted
        canvas.drawText(caption, cx, cy + 6f * scale, cap)
    }

    private fun attrColor(attr: Int, fallback: Int): Int {
        val tv = TypedValue()
        return if (context.theme.resolveAttribute(attr, tv, true)) tv.data else fallback
    }

    private fun withAlpha(color: Int, a: Float): Int =
        Color.argb((a * 255).toInt(), Color.red(color), Color.green(color), Color.blue(color))
}
