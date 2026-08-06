package com.loadtester.el15

import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.pdf.PdfDocument
import java.io.ByteArrayOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.log10
import kotlin.math.pow

/**
 * Printable PDF reports for both test engines.
 *
 * Drawn straight onto [PdfDocument] page canvases rather than paginating a
 * screenshot, so text stays selectable and the charts stay crisp at any zoom.
 *
 * The report is deliberately a LIGHT document even though the app is dark: this
 * is made to be printed and filed, and a dark page is unreadable on paper and
 * ruinous on toner. Colours are the validated light-surface palette.
 *
 * Chart rules followed here, and worth keeping if this is extended:
 *  - No dual-axis plots ever. Two measures of different scale become two stacked
 *    charts sharing an x axis (small multiples), because aligning two y scales
 *    invents a correlation the data does not contain.
 *  - Hairline, recessive grid and axes; solid, never dashed — a dashed grid reads
 *    as a threshold when it is only a grid.
 *  - Selective direct labels, never a number on every point.
 *  - The detail tables are the report's table view, so no value is reachable only
 *    by reading a chart.
 */
object PdfReport {

    // ---- Page geometry (A4 at 72 dpi) ----------------------------------------
    private const val PAGE_W = 595
    private const val PAGE_H = 842
    private const val MARGIN_X = 44f
    private const val MARGIN_TOP = 46f
    private const val MARGIN_BOTTOM = 52f
    private val CONTENT_W = PAGE_W - 2 * MARGIN_X

    // ---- Palette (validated light surface) -----------------------------------
    private const val PAGE_BG = 0xFFFFFFFF.toInt()
    private const val SURFACE = 0xFFFCFCFB.toInt()
    private const val INK = 0xFF0B0B0B.toInt()
    private const val INK_2 = 0xFF52514E.toInt()
    private const val MUTED = 0xFF898781.toInt()
    private const val GRID = 0xFFE1E0D9.toInt()
    private const val AXIS = 0xFFC3C2B7.toInt()

    // Categorical slots 1-3 — the only three that clear the all-pairs separation
    // floors, which is the bar that applies here because these charts are small
    // multiples rather than series sharing one frame. A fourth entity would need
    // a fourth hue, and every candidate collides: green against orange measures
    // ΔE 3.2 under protanopia. So the report carries three quantities and the
    // rest live in the tables.
    //
    // Colour follows the QUANTITY, not the chart: voltage is always blue, current
    // always orange, capacity always aqua, so flipping between plots is safe.
    private const val V_BLUE = 0xFF2A78D6.toInt()
    private const val I_ORANGE = 0xFFEB6834.toInt()
    // Aqua measures 2.74:1 on this surface, under the 3:1 bar. The sanctioned
    // relief applies and is present: the series is named in the chart title and
    // the y-axis label, and every value it plots is also in the tables above.
    private const val AH_AQUA = 0xFF1BAF7A.toInt()

    private const val GOOD = 0xFF0CA30C.toInt()
    private const val WARN = 0xFFFAB219.toInt()
    private const val CRITICAL = 0xFFD03B3B.toInt()

    private val SANS: Typeface = Typeface.create("sans-serif", Typeface.NORMAL)
    private val SANS_BOLD: Typeface = Typeface.create("sans-serif", Typeface.BOLD)
    private val MONO: Typeface = Typeface.MONOSPACE

    // ---- Public entry points --------------------------------------------------
    fun rTest(r: ResistanceTest.Result): ByteArray {
        val s = Sheet("Circuit resistance test")
        s.hero(
            "Circuit resistance",
            DeviceCore.formatOhm(r.resistanceOhm),
            buildString {
                if (r.resistanceStdErr > 0f) {
                    append("± ${DeviceCore.formatOhm(r.resistanceStdErr)} (1σ)   ·   ")
                }
                append("Voc ${fmt(r.openCircuitVoltage, 3)} V   ·   R² ${fmt(r.rSquared, 5)}")
            },
            if (r.reliable) "Reliable — the fit's uncertainty is within tolerance"
            else "Low confidence — widen the current span or lengthen the sweep",
            if (r.reliable) GOOD else WARN,
        )

        s.section("Result")
        s.table(
            listOf(
                "Resistance" to DeviceCore.formatOhm(r.resistanceOhm),
                "Uncertainty (1σ)" to DeviceCore.formatOhm(r.resistanceStdErr),
                "Measured before tare" to DeviceCore.formatOhm(r.rawResistanceOhm),
                "Open-circuit voltage" to "${fmt(r.openCircuitVoltage, 4)} V",
                "R²" to fmt(r.rSquared, 5),
                "Est. short-circuit current" to
                    if (r.resistanceOhm > 1e-6f) "${fmt(r.openCircuitVoltage / r.resistanceOhm, 1)} A"
                    else "n/a",
                "Voltage sag" to "${fmt(r.sagV, 4)} V",
                "Peak power" to "${fmt(r.peakPowerW, 2)} W",
            )
        )

        s.section("Test conditions")
        s.table(
            listOf(
                "Probe wiring" to when {
                    r.fourWire -> "4-wire (Kelvin)"
                    r.tareOhm > 0f -> "2-wire, tare ${DeviceCore.formatOhm(r.tareOhm)}"
                    else -> "2-wire, no tare"
                },
                "Fuse rating" to "${fmt(r.fuseRating, 2)} A",
                "Sweep" to "${fmt(r.startCurrent, 3)} → ${fmt(r.maxTestCurrent, 3)} A " +
                    "over ${r.sweepSeconds} s",
                "Ramp shape" to "eased triangle, up then down",
                "Current measured" to "${fmt(r.minCurrent, 4)} → ${fmt(r.maxCurrentSeen, 4)} A",
                "Samples fitted" to "${r.rawSamples} in ${r.samples.size} bands",
                "Excluded, step transient" to r.excludedTransient.toString(),
                "Excluded, repeat frame" to r.excludedDuplicate.toString(),
                "Excluded, off target" to r.excludedOffTarget.toString(),
                "Load dropouts" to r.loadDropouts.toString(),
                "Temperature" to "${fmt(r.tempMin, 1)} → ${fmt(r.tempMax, 1)} °C",
                "Max fan" to "${r.maxFan} of ${El15Protocol.FAN_SPEED_MAX}",
            )
        )

        // ---- Hero chart: the measurement itself -------------------------------
        // Both axes carry data, so this is the one plot where two quantities
        // legitimately share a frame — it IS the V-I relationship being measured.
        if (r.samples.size >= 2) {
            val pts = r.samples.map { it.current to it.voltage }
            val iMin = pts.minOf { it.first }
            val iMax = pts.maxOf { it.first }
            val fitLine = listOf(
                iMin to r.openCircuitVoltage - r.rawResistanceOhm * iMin,
                iMax to r.openCircuitVoltage - r.rawResistanceOhm * iMax,
            )
            s.chart(
                title = "Voltage against current, with the fitted line",
                caption = "Each point averages both ramp directions in one current band. " +
                    "The slope is the resistance.",
                xLabel = "Current (A)", yLabel = "Voltage (V)",
                series = listOf(
                    Series("Fitted line", I_ORANGE, fitLine, Style.LINE),
                    Series("Measured", V_BLUE, pts, Style.SCATTER),
                ),
                height = 218f,
            )

            // Residuals expose curvature that R² hides — a bowed pattern means the
            // circuit is not ohmic over the swept range, which no single number
            // can tell you.
            val resid = r.samples.map {
                it.current to (it.voltage - (r.openCircuitVoltage - r.rawResistanceOhm * it.current))
            }
            s.chart(
                title = "Fit residuals",
                caption = "Measured minus fitted voltage. Scatter about zero is noise; " +
                    "a systematic bow means the circuit is not ohmic across the sweep.",
                xLabel = "Current (A)", yLabel = "Residual (V)",
                series = listOf(Series("Residual", V_BLUE, resid, Style.SCATTER)),
                refLinesY = listOf(RefLine(0.0, "zero", AXIS)),
                height = 168f,
            )
        }

        // ---- Sweep shape, from the raw datapoint log ---------------------------
        val log = SampleLog.rtest
        if (log.count > 1) {
            val measured = ArrayList<Pair<Float, Float>>()
            val target = ArrayList<Pair<Float, Float>>()
            val volts = ArrayList<Pair<Float, Float>>()
            log.replay { rec ->
                val t = rec.tMs / 1000f
                measured.add(t to rec.i)
                target.add(t to rec.aux0)
                volts.add(t to rec.v)
                true
            }
            s.chart(
                title = "Current through the sweep",
                caption = "Commanded against measured. The gap between them is the load's " +
                    "regulation lag, which the fit is immune to — voltage and current inside " +
                    "one frame are simultaneous.",
                xLabel = "Elapsed (s)", yLabel = "Current (A)",
                series = listOf(
                    Series("Commanded", MUTED, target, Style.LINE),
                    Series("Measured", I_ORANGE, measured, Style.LINE),
                ),
                height = 168f,
            )
            s.chart(
                title = "Terminal voltage through the sweep",
                caption = "Sag under load, and its recovery on the way back down.",
                xLabel = "Elapsed (s)", yLabel = "Voltage (V)",
                series = listOf(Series("Voltage", V_BLUE, volts, Style.LINE)),
                height = 168f,
            )
        }

        s.methodNote(
            "The sweep ramps the load's current up and back down as a smooth triangle and " +
                "fits every settled reading by least squares, so each current is visited once " +
                "in each direction and first-order drift — self-heating, a sagging source — " +
                "cancels instead of biasing the slope. The ± figure is the standard error of " +
                "that slope, not a guess. Readings taken inside a setpoint step's response, " +
                "and frames the device repeated between refreshes, are excluded from the fit " +
                "and counted above; the CSV export keeps every packet."
        )
        return s.finish()
    }

    fun battery(r: CapacityTest.Result): ByteArray {
        val s = Sheet("Battery capacity test")
        val capped = r.stopReason.contains("cap", true) || r.stopReason.contains("Protection", true)
        s.hero(
            "Measured capacity",
            "${fmt(r.capacityAh, 3)} Ah",
            "${fmt(r.capacityAh * 1000f, 0)} mAh   ·   ${fmt(r.energyWh, 2)} Wh   ·   " +
                DeviceCore.formatDuration(r.durationS),
            r.stopReason,
            if (capped) WARN else GOOD,
        )

        s.section("Result")
        val result = ArrayList<Pair<String, String>>()
        result += "Capacity" to "${fmt(r.capacityAh, 4)} Ah  (${fmt(r.capacityAh * 1000f, 0)} mAh)"
        result += "Energy" to "${fmt(r.energyWh, 3)} Wh"
        result += "Duration" to DeviceCore.formatDuration(r.durationS)
        if (r.pausedS > 0) result += "Paused" to DeviceCore.formatDuration(r.pausedS)
        result += "Stop reason" to r.stopReason
        result += "Start voltage" to "${fmt(r.startV, 3)} V"
        result += "End voltage" to "${fmt(r.endV, 3)} V"
        result += "Rebound voltage" to "${fmt(r.reboundV, 3)} V"
        result += "Average" to "${fmt(r.avgV, 3)} V at ${fmt(r.avgI, 3)} A"
        if (r.ratedAh > 0f) {
            result += "Rated capacity" to "${fmt(r.ratedAh, 3)} Ah"
            result += "State of health" to "${fmt(r.sohPct, 1)} %"
            result += "Discharge rate" to "${fmt(r.cRate, 3)} C"
        }
        if (r.internalResistanceOhm > 0f) {
            result += "Pack resistance" to DeviceCore.formatOhm(r.internalResistanceOhm)
        }
        if (r.startSocPct >= 0f) {
            result += "Charge state" to "~${fmt(r.startSocPct, 0)} % → ~${fmt(r.endSocPct, 0)} %"
        }
        if (r.impliedFullAh > 0f) {
            result += "Implied full capacity" to "${fmt(r.impliedFullAh, 3)} Ah"
        }
        s.table(result)

        s.section("Test conditions")
        val chem = BatteryModel.CHEMS.getOrNull(r.chemistry)
        val cond = ArrayList<Pair<String, String>>()
        if (chem != null) {
            cond += "Chemistry" to chem.name + if (r.cells > 0) "  ·  ${r.cells}S" else ""
        }
        cond += "Discharge current" to "${fmt(r.currentA, 3)} A"
        cond += "Cutoff voltage" to "${fmt(r.cutoffV, 2)} V"
        cond += "Probe wiring" to
            if (r.fourWire) "4-wire (Kelvin)" else "2-wire, tare ${DeviceCore.formatOhm(r.tareOhm)}"
        cond += "Temperature" to "${fmt(r.minTemp, 1)} → ${fmt(r.maxTemp, 1)} °C"
        cond += "Max fan" to "${r.maxFan} of ${El15Protocol.FAN_SPEED_MAX}"
        cond += "Safety cap, capacity" to "${fmt(r.capAh, 1)} Ah"
        cond += "Safety cap, duration" to "${fmt(r.capDurationS / 3600f, 1)} h"
        s.table(cond)

        // ---- Small multiples over a shared x axis -----------------------------
        // Voltage, current, capacity and temperature have unrelated scales, so
        // they get their own frames rather than sharing one with several y axes.
        val log = SampleLog.batt
        if (log.count > 1) {
            val vByAh = ArrayList<Pair<Float, Float>>()
            val vByT = ArrayList<Pair<Float, Float>>()
            val iByT = ArrayList<Pair<Float, Float>>()
            val ahByT = ArrayList<Pair<Float, Float>>()
            log.replay { rec ->
                val h = rec.tMs / 3_600_000f
                vByAh.add(rec.aux0 to rec.v)
                vByT.add(h to rec.v)
                iByT.add(h to rec.i)
                ahByT.add(h to rec.aux0)
                true
            }

            s.chart(
                title = "Discharge curve",
                caption = "Terminal voltage against the capacity drawn out of the pack — " +
                    "the shape that shows how the pack behaves, not just how much it held.",
                xLabel = "Capacity drawn (Ah)", yLabel = "Voltage (V)",
                series = listOf(Series("Voltage", V_BLUE, vByAh, Style.LINE)),
                refLinesY = listOf(RefLine(r.cutoffV.toDouble(), "cutoff", CRITICAL)),
                height = 218f,
            )
            s.chart(
                title = "Voltage over time",
                xLabel = "Elapsed (h)", yLabel = "Voltage (V)",
                series = listOf(Series("Voltage", V_BLUE, vByT, Style.LINE)),
                refLinesY = listOf(RefLine(r.cutoffV.toDouble(), "cutoff", CRITICAL)),
                height = 156f,
            )
            s.chart(
                title = "Discharge current over time",
                caption = "Flat is healthy: the load holds constant current until the cutoff.",
                xLabel = "Elapsed (h)", yLabel = "Current (A)",
                series = listOf(Series("Current", I_ORANGE, iByT, Style.LINE)),
                height = 156f,
            )
            s.chart(
                title = "Capacity accumulated",
                xLabel = "Elapsed (h)", yLabel = "Capacity (Ah)",
                series = listOf(Series("Capacity", AH_AQUA, ahByT, Style.LINE)),
                height = 156f,
            )
        }

        s.methodNote(
            "Capacity is integrated locally from the load's own voltage and current readings " +
                "rather than read from its counter, and the run stops on a debounced cutoff. " +
                "Where a state of charge is shown it is read off a representative curve for " +
                "the chemistry, corrected through the pack resistance measured from the " +
                "switch-on sag — good enough to estimate time remaining, not good enough to " +
                "report as a measurement, which is why it is prefixed \"~\". A run that ended " +
                "on a safety cap is the guard firing, not the pack's capacity."
        )
        return s.finish()
    }

    // ---- Drawing primitives ----------------------------------------------------
    private enum class Style { LINE, SCATTER }

    private class Series(
        val label: String,
        val color: Int,
        val points: List<Pair<Float, Float>>,
        val style: Style,
    )

    private class RefLine(val value: Double, val label: String, val color: Int)

    /**
     * A flowing document: content is appended top to bottom and spills onto a new
     * page when it runs out of room, so nothing has to be laid out by hand.
     */
    private class Sheet(private val docTitle: String) {
        private val doc = PdfDocument()
        private var page: PdfDocument.Page? = null
        private var c: Canvas? = null
        private var y = 0f
        private var pageNo = 0
        private val stamp: String =
            SimpleDateFormat("d MMM yyyy, HH:mm", Locale.getDefault()).format(Date())

        private val p = Paint(Paint.ANTI_ALIAS_FLAG)

        init { newPage() }

        private fun canvas(): Canvas = c!!

        private fun newPage() {
            page?.let { doc.finishPage(it) }
            pageNo++
            val info = PdfDocument.PageInfo.Builder(PAGE_W, PAGE_H, pageNo).create()
            val pg = doc.startPage(info)
            page = pg
            c = pg.canvas
            canvas().drawColor(PAGE_BG)
            drawRunningHead()
            y = MARGIN_TOP + 30f
        }

        private fun drawRunningHead() {
            val cv = canvas()
            p.reset(); p.isAntiAlias = true
            p.typeface = SANS_BOLD; p.textSize = 9f; p.color = INK
            cv.drawText("EL15 · $docTitle", MARGIN_X, MARGIN_TOP, p)
            p.typeface = SANS; p.color = MUTED
            p.textAlign = Paint.Align.RIGHT
            cv.drawText(stamp, PAGE_W - MARGIN_X, MARGIN_TOP, p)
            p.textAlign = Paint.Align.LEFT
            p.color = AXIS; p.strokeWidth = 0.6f
            cv.drawLine(MARGIN_X, MARGIN_TOP + 8f, PAGE_W - MARGIN_X, MARGIN_TOP + 8f, p)

            p.color = MUTED; p.textSize = 8f; p.textAlign = Paint.Align.CENTER
            cv.drawText(
                "EL15 Load Control  ·  page $pageNo",
                PAGE_W / 2f, PAGE_H - MARGIN_BOTTOM + 22f, p,
            )
            p.textAlign = Paint.Align.LEFT
        }

        /** Start a new page if [h] more points won't fit below the current cursor. */
        private fun need(h: Float) {
            if (y + h > PAGE_H - MARGIN_BOTTOM) newPage()
        }

        // ---- Blocks ------------------------------------------------------------
        fun hero(label: String, value: String, sub: String, status: String, statusColor: Int) {
            need(104f)
            val cv = canvas()
            p.reset(); p.isAntiAlias = true

            p.color = MUTED; p.typeface = SANS_BOLD; p.textSize = 8.5f
            cv.drawText(label.uppercase(Locale.US), MARGIN_X, y, p)
            y += 30f

            // Proportional figures on the hero number: tabular digits make a large
            // standalone value look loosely spaced.
            p.color = INK; p.typeface = SANS_BOLD; p.textSize = 30f
            cv.drawText(value, MARGIN_X, y, p)
            y += 16f

            p.color = INK_2; p.typeface = SANS; p.textSize = 9.5f
            cv.drawText(sub, MARGIN_X, y, p)
            y += 20f

            // Status carries an explicit label, never colour alone.
            p.color = statusColor
            cv.drawCircle(MARGIN_X + 3f, y - 3f, 3f, p)
            p.color = INK_2; p.textSize = 9f
            cv.drawText(status, MARGIN_X + 12f, y, p)
            y += 22f
        }

        fun section(title: String) {
            need(34f)
            val cv = canvas()
            p.reset(); p.isAntiAlias = true
            y += 8f
            p.color = INK; p.typeface = SANS_BOLD; p.textSize = 10.5f
            cv.drawText(title, MARGIN_X, y, p)
            y += 6f
            p.color = AXIS; p.strokeWidth = 0.6f
            cv.drawLine(MARGIN_X, y, PAGE_W - MARGIN_X, y, p)
            y += 14f
        }

        /**
         * Two-column label/value table. Monospace values so digits line up down the
         * column — this is where tabular figures belong.
         */
        fun table(rows: List<Pair<String, String>>) {
            val colW = CONTENT_W / 2f
            val rowH = 15f
            var i = 0
            while (i < rows.size) {
                need(rowH)
                val cv = canvas()
                for (col in 0 until 2) {
                    val idx = i + col
                    if (idx >= rows.size) break
                    val (k, v) = rows[idx]
                    val x = MARGIN_X + col * colW
                    p.reset(); p.isAntiAlias = true
                    p.color = INK_2; p.typeface = SANS; p.textSize = 8.5f
                    cv.drawText(k, x, y, p)
                    p.color = INK; p.typeface = MONO; p.textSize = 8.5f
                    cv.drawText(v, x + 128f, y, p)
                }
                y += rowH
                i += 2
            }
            y += 6f
        }

        fun methodNote(text: String) {
            need(70f)
            y += 10f
            val cv = canvas()
            p.reset(); p.isAntiAlias = true
            p.color = MUTED; p.typeface = SANS_BOLD; p.textSize = 8f
            cv.drawText("METHOD", MARGIN_X, y, p)
            y += 12f
            p.typeface = SANS; p.textSize = 8.5f; p.color = INK_2
            for (line in wrap(text, p, CONTENT_W)) {
                need(11f)
                canvas().drawText(line, MARGIN_X, y, p)
                y += 11f
            }
        }

        // ---- Charts -------------------------------------------------------------
        fun chart(
            title: String,
            xLabel: String,
            yLabel: String,
            series: List<Series>,
            caption: String? = null,
            refLinesY: List<RefLine> = emptyList(),
            height: Float = 190f,
        ) {
            val usable = series.filter { it.points.size >= 2 || it.style == Style.SCATTER }
                .filter { it.points.isNotEmpty() }
            if (usable.isEmpty()) return

            val captionLines = caption?.let {
                p.reset(); p.isAntiAlias = true; p.typeface = SANS; p.textSize = 8f
                wrap(it, p, CONTENT_W)
            } ?: emptyList()
            val total = height + captionLines.size * 10f + 16f
            need(total)

            val cv = canvas()
            val card = RectF(MARGIN_X, y, PAGE_W - MARGIN_X, y + height)

            p.reset(); p.isAntiAlias = true
            p.color = SURFACE; p.style = Paint.Style.FILL
            cv.drawRect(card, p)
            p.color = GRID; p.style = Paint.Style.STROKE; p.strokeWidth = 0.6f
            cv.drawRect(card, p)
            p.style = Paint.Style.FILL

            p.color = INK; p.typeface = SANS_BOLD; p.textSize = 9.5f
            cv.drawText(title, card.left + 12f, card.top + 17f, p)

            // A legend is present whenever more than one series shares the frame,
            // so identity is never carried by colour alone.
            if (usable.size >= 2) drawLegend(cv, card, usable)

            val plot = RectF(
                card.left + 52f, card.top + 28f,
                card.right - 14f, card.bottom - 26f,
            )
            drawPlot(cv, plot, usable, refLinesY, xLabel, yLabel)

            y = card.bottom + 12f
            if (captionLines.isNotEmpty()) {
                p.reset(); p.isAntiAlias = true
                p.color = MUTED; p.typeface = SANS; p.textSize = 8f
                for (line in captionLines) {
                    cv.drawText(line, MARGIN_X, y, p)
                    y += 10f
                }
                y += 4f
            }
        }

        private fun drawLegend(cv: Canvas, card: RectF, series: List<Series>) {
            p.reset(); p.isAntiAlias = true
            p.typeface = SANS; p.textSize = 8f
            var x = card.right - 12f
            for (s in series.asReversed()) {
                val w = p.measureText(s.label)
                p.color = INK_2
                cv.drawText(s.label, x - w, card.top + 17f, p)
                p.color = s.color
                p.strokeWidth = 2f
                cv.drawLine(x - w - 14f, card.top + 14f, x - w - 4f, card.top + 14f, p)
                x -= w + 24f
            }
        }

        private fun drawPlot(
            cv: Canvas, plot: RectF, series: List<Series>,
            refLinesY: List<RefLine>, xLabel: String, yLabel: String,
        ) {
            var xMin = Double.MAX_VALUE; var xMax = -Double.MAX_VALUE
            var yMin = Double.MAX_VALUE; var yMax = -Double.MAX_VALUE
            for (s in series) for ((px, py) in s.points) {
                if (px < xMin) xMin = px.toDouble(); if (px > xMax) xMax = px.toDouble()
                if (py < yMin) yMin = py.toDouble(); if (py > yMax) yMax = py.toDouble()
            }
            for (rl in refLinesY) {
                if (rl.value < yMin) yMin = rl.value
                if (rl.value > yMax) yMax = rl.value
            }
            if (xMax - xMin < 1e-9) { xMin -= 0.5; xMax += 0.5 }
            if (yMax - yMin < 1e-9) { yMin -= 0.5; yMax += 0.5 }
            // Breathing room, so marks never sit on the frame.
            val padY = (yMax - yMin) * 0.08
            yMin -= padY; yMax += padY

            val xTicks = niceTicks(xMin, xMax, 5)
            val yTicks = niceTicks(yMin, yMax, 5)
            xMin = minOf(xMin, xTicks.first()); xMax = maxOf(xMax, xTicks.last())
            yMin = minOf(yMin, yTicks.first()); yMax = maxOf(yMax, yTicks.last())

            fun sx(v: Double) = (plot.left + (v - xMin) / (xMax - xMin) * plot.width()).toFloat()
            fun sy(v: Double) = (plot.bottom - (v - yMin) / (yMax - yMin) * plot.height()).toFloat()

            // Grid: solid hairlines a shade off the surface. Never dashed — dashing
            // reads as a threshold when it is only a grid.
            p.reset(); p.isAntiAlias = true
            p.strokeWidth = 0.5f
            for (t in yTicks) {
                p.color = GRID
                cv.drawLine(plot.left, sy(t), plot.right, sy(t), p)
            }
            p.color = AXIS; p.strokeWidth = 0.8f
            cv.drawLine(plot.left, plot.bottom, plot.right, plot.bottom, p)
            cv.drawLine(plot.left, plot.top, plot.left, plot.bottom, p)

            // Tick labels: tabular figures, since these align vertically.
            p.typeface = MONO; p.textSize = 7f; p.color = MUTED
            p.textAlign = Paint.Align.RIGHT
            for (t in yTicks) cv.drawText(tick(t), plot.left - 5f, sy(t) + 2.5f, p)
            p.textAlign = Paint.Align.CENTER
            for (t in xTicks) cv.drawText(tick(t), sx(t), plot.bottom + 11f, p)
            p.textAlign = Paint.Align.LEFT

            p.typeface = SANS; p.textSize = 7.5f; p.color = INK_2
            p.textAlign = Paint.Align.CENTER
            cv.drawText(xLabel, plot.centerX(), plot.bottom + 21f, p)
            cv.save()
            cv.rotate(-90f, plot.left - 38f, plot.centerY())
            cv.drawText(yLabel, plot.left - 38f, plot.centerY(), p)
            cv.restore()
            p.textAlign = Paint.Align.LEFT

            // Reference lines carry a label, so they never rely on colour alone.
            for (rl in refLinesY) {
                if (rl.value < yMin || rl.value > yMax) continue
                p.color = rl.color; p.strokeWidth = 0.9f
                cv.drawLine(plot.left, sy(rl.value), plot.right, sy(rl.value), p)
                p.typeface = SANS; p.textSize = 7f
                cv.drawText(rl.label, plot.left + 4f, sy(rl.value) - 3f, p)
            }

            for (s in series) {
                when (s.style) {
                    Style.LINE -> {
                        val path = Path()
                        var started = false
                        for ((px, py) in s.points) {
                            val X = sx(px.toDouble()); val Y = sy(py.toDouble())
                            if (!started) { path.moveTo(X, Y); started = true } else path.lineTo(X, Y)
                        }
                        p.reset(); p.isAntiAlias = true
                        p.style = Paint.Style.STROKE
                        p.strokeWidth = 1.6f
                        p.strokeCap = Paint.Cap.ROUND
                        p.strokeJoin = Paint.Join.ROUND
                        p.color = s.color
                        cv.drawPath(path, p)
                        p.style = Paint.Style.FILL
                    }
                    Style.SCATTER -> {
                        for ((px, py) in s.points) {
                            val X = sx(px.toDouble()); val Y = sy(py.toDouble())
                            // 2px surface ring so overlapping marks stay countable
                            // without drawing a border around every one.
                            p.reset(); p.isAntiAlias = true
                            p.color = SURFACE
                            cv.drawCircle(X, Y, 4.6f, p)
                            p.color = s.color
                            cv.drawCircle(X, Y, 3.2f, p)
                        }
                    }
                }
            }
        }

        fun finish(): ByteArray {
            page?.let { doc.finishPage(it) }
            page = null
            val out = ByteArrayOutputStream()
            doc.writeTo(out)
            doc.close()
            return out.toByteArray()
        }
    }

    // ---- Helpers ---------------------------------------------------------------
    private fun fmt(v: Float, dp: Int): String = String.format(Locale.US, "%.${dp}f", v)

    private fun tick(v: Double): String {
        val a = abs(v)
        return when {
            a >= 1000 -> String.format(Locale.US, "%.0f", v)
            a >= 10 -> String.format(Locale.US, "%.1f", v)
            a >= 1 -> String.format(Locale.US, "%.2f", v)
            a >= 0.01 -> String.format(Locale.US, "%.3f", v)
            v == 0.0 -> "0"
            else -> String.format(Locale.US, "%.4f", v)
        }
    }

    /** Round tick positions, so axes read 0.5 / 1.0 / 1.5 rather than 0.4713. */
    private fun niceTicks(min: Double, max: Double, target: Int): List<Double> {
        if (max <= min) return listOf(min, max)
        val raw = (max - min) / target
        val mag = 10.0.pow(floor(log10(raw)))
        val norm = raw / mag
        val step = when {
            norm <= 1.0 -> 1.0
            norm <= 2.0 -> 2.0
            norm <= 2.5 -> 2.5
            norm <= 5.0 -> 5.0
            else -> 10.0
        } * mag
        val first = floor(min / step) * step
        val out = ArrayList<Double>()
        var t = first
        var guard = 0
        while (t <= max + step * 0.5 && guard++ < 64) { out.add(t); t += step }
        return if (out.size >= 2) out else listOf(min, max)
    }

    private fun wrap(text: String, paint: Paint, width: Float): List<String> {
        val words = text.split(' ')
        val lines = ArrayList<String>()
        var line = StringBuilder()
        for (w in words) {
            val candidate = if (line.isEmpty()) w else "$line $w"
            if (paint.measureText(candidate) > width && line.isNotEmpty()) {
                lines.add(line.toString()); line = StringBuilder(w)
            } else {
                line = StringBuilder(candidate)
            }
        }
        if (line.isNotEmpty()) lines.add(line.toString())
        return lines
    }
}
