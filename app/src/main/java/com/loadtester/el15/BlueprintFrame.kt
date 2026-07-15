package com.loadtester.el15

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.util.TypedValue
import android.widget.FrameLayout

/**
 * A square blueprint panel: transparent/surface fill, 1px hairline border, and a
 * small "+" registration mark at each corner — the core Industry-blueprint frame.
 */
class BlueprintFrame @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : FrameLayout(context, attrs) {

    private val border = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val cross = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }

    init {
        setWillNotDraw(false)
        val d = resources.displayMetrics.density
        border.strokeWidth = d
        cross.strokeWidth = d
        border.color = attrColor(com.google.android.material.R.attr.colorOutline, Color.GRAY)
        cross.color = withAlpha(attrColor(com.google.android.material.R.attr.colorOnSurface, Color.LTGRAY), 0.55f)
    }

    override fun dispatchDraw(canvas: Canvas) {
        super.dispatchDraw(canvas)
        val i = 0.5f * resources.displayMetrics.density
        canvas.drawRect(i, i, width - i, height - i, border)
        val pad = dp(6f)
        val half = dp(3.5f)
        drawCross(canvas, pad, pad, half)
        drawCross(canvas, width - pad, pad, half)
        drawCross(canvas, pad, height - pad, half)
        drawCross(canvas, width - pad, height - pad, half)
    }

    private fun drawCross(c: Canvas, cx: Float, cy: Float, half: Float) {
        c.drawLine(cx - half, cy, cx + half, cy, cross)
        c.drawLine(cx, cy - half, cx, cy + half, cross)
    }

    private fun attrColor(attr: Int, fallback: Int): Int {
        val tv = TypedValue()
        return if (context.theme.resolveAttribute(attr, tv, true)) tv.data else fallback
    }

    private fun withAlpha(color: Int, a: Float): Int =
        Color.argb((a * 255).toInt(), Color.red(color), Color.green(color), Color.blue(color))

    private fun dp(v: Float) = v * resources.displayMetrics.density
}
