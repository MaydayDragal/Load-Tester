package com.loadtester.el15

/**
 * Battery chemistry model — per-cell open-circuit voltage curves, pack voltage
 * limits, and the standard test C-rates that go with them.
 *
 * Kotlin port of the firmware's `battery_model.h`, kept deliberately
 * line-for-line comparable so the two stay in step.
 *
 * Two things depend on this, and both used to be guesses:
 *
 *  1. TIME REMAINING. The old estimate was pure coulomb counting against the
 *     nameplate rating — "(rated - drawn) / current" — which silently assumes
 *     the pack started full and that the test ends when the rating is reached.
 *     Neither is true: a half-charged pack showed roughly double the real time,
 *     and the test actually ends at the CUTOFF VOLTAGE, which on a high cutoff
 *     can be nowhere near empty. Reading state of charge off the chemistry's
 *     voltage curve fixes both, and needs no rating at all.
 *
 *  2. DISCHARGE CURRENT. Capacity is only meaningful at a stated rate, and every
 *     chemistry has a conventional one (IEC rates Li-ion and NiMH at 0.2C;
 *     lead-acid is rated at the C20 = 0.05C hour rate; alkaline cells are rated
 *     at a low continuous drain). Given the pack's size the app can work the
 *     current out instead of the user doing it.
 *
 * The curves are RESTED open-circuit voltages, so a reading taken under load has
 * to be referred back through the pack's internal resistance before it can be
 * looked up — see [CapacityTest.updateModel], which measures that resistance
 * from the switch-on sag rather than assuming a value.
 *
 * Accuracy, honestly: these are representative curves for each family, not for
 * your specific cell. They are good enough for a time estimate that improves as
 * the run proceeds (the engine learns the pack's real capacity from Ah-drawn per
 * unit of SoC travelled and stops relying on the curve's absolute calibration),
 * and they are NOT good enough to report as a state-of-charge measurement. That
 * is why every SoC shown in the UI is prefixed "~".
 *
 * Every maxCells below is chosen so a FULLY CHARGED pack of that size stays
 * inside the EL15's 60 V input rating — the engine refuses anything above it, so
 * offering a cell count that cannot be tested would be a trap.
 */
object BatteryModel {

    /**
     * OCV samples per curve, evenly spaced over state of charge: index k is
     * k/(OCV_N-1) of full, so index 0 is empty and index 10 is full. Even
     * spacing keeps the lookup a single divide and the table readable.
     */
    const val OCV_N = 11

    /** Offered test C-rates per chemistry, ascending. */
    const val CRATE_N = 4

    data class Chem(
        /** Full name, for the detail line. */
        val name: String,
        /** Chip label — keep to ~8 characters. */
        val shortName: String,
        /** Per cell: nominal voltage. */
        val nom: Float,
        /** Per cell: fully charged voltage. */
        val full: Float,
        /** Per cell: discharge cutoff voltage. */
        val cut: Float,
        /** Largest series count that stays under 60 V; 0 = no cell model. */
        val maxCells: Int,
        /** Non-zero: the pack size is fixed, not user-selectable. */
        val fixedCells: Int,
        /** Rested per-cell OCV, empty -> full. MUST be ascending. */
        val ocv: FloatArray,
        /** Selectable test rates. */
        val cRate: FloatArray,
        /** Index of the rate the chemistry is conventionally rated at. */
        val stdRate: Int,
    )

    val CHEMS: List<Chem> = listOf(
        // Li-ion (NMC / LCO / most 18650s). Sloped curve throughout — the easiest
        // chemistry to read a state of charge from, and the one where a
        // voltage-based estimate is genuinely trustworthy.
        Chem(
            "Li-ion (NMC/LCO)", "Li-ion", 3.7f, 4.2f, 3.0f, 14, 0,
            floatArrayOf(3.00f, 3.45f, 3.57f, 3.63f, 3.68f, 3.73f, 3.79f, 3.87f, 3.95f, 4.06f, 4.20f),
            floatArrayOf(0.1f, 0.2f, 0.5f, 1.0f), 1,
        ),
        // High-voltage LiPo (LiHV), charged to 4.35 V — common in RC packs. Same
        // shape as Li-ion with the top of the curve stretched; using the plain
        // Li-ion curve on one of these would read a full pack as over 100 % and
        // clamp, losing the whole top of the discharge.
        Chem(
            "LiPo HV (4.35 V)", "LiHV", 3.8f, 4.35f, 3.0f, 13, 0,
            floatArrayOf(3.00f, 3.45f, 3.57f, 3.64f, 3.70f, 3.76f, 3.83f, 3.91f, 4.01f, 4.15f, 4.35f),
            floatArrayOf(0.1f, 0.2f, 0.5f, 1.0f), 1,
        ),
        // LiFePO4. The plateau between ~20 % and ~90 % spans about 150 mV, so
        // voltage says very little about charge state across most of the run. The
        // estimate leans hard on the learned-capacity term here, and the SoC
        // figure is worth treating as a rough indication until the knee arrives.
        Chem(
            "LiFePO4", "LiFePO4", 3.2f, 3.65f, 2.5f, 16, 0,
            floatArrayOf(2.50f, 3.00f, 3.15f, 3.22f, 3.25f, 3.27f, 3.29f, 3.30f, 3.31f, 3.33f, 3.45f),
            floatArrayOf(0.1f, 0.2f, 0.5f, 1.0f), 1,
        ),
        // Lithium titanate. Low cell voltage, very flat, extremely tolerant of
        // high rates — hence the higher C-rate presets.
        Chem(
            "LTO (titanate)", "LTO", 2.4f, 2.8f, 1.8f, 21, 0,
            floatArrayOf(1.80f, 2.10f, 2.20f, 2.25f, 2.28f, 2.31f, 2.34f, 2.38f, 2.44f, 2.52f, 2.80f),
            floatArrayOf(0.2f, 0.5f, 1.0f, 2.0f), 1,
        ),
        // Sodium-ion. Wide, strongly sloped working range (roughly 1.5-4.0 V),
        // which makes it one of the better chemistries for a voltage-based
        // charge state.
        Chem(
            "Sodium-ion", "Na-ion", 3.1f, 4.0f, 1.5f, 14, 0,
            floatArrayOf(1.50f, 2.30f, 2.70f, 2.90f, 3.05f, 3.18f, 3.30f, 3.45f, 3.60f, 3.78f, 4.00f),
            floatArrayOf(0.1f, 0.2f, 0.5f, 1.0f), 1,
        ),
        // Lead-acid, FIXED at 12 V (six 2 V cells) — the only size worth offering
        // here, so the cell-count control disappears for it and the cutoff lands
        // on the standard 10.5 V straight away. Near-linear OCV vs charge state,
        // which makes it the textbook case for this method. Note ocv[0] is
        // 1.90 V/cell and not the 1.75 V cutoff: 1.75 is the LOADED
        // end-of-discharge voltage, while a rested flat cell sits near 1.90.
        // Rated at the C20 (0.05C) hour rate by convention, so that is default.
        Chem(
            "Lead-acid 12 V", "Pb 12V", 2.0f, 2.13f, 1.75f, 6, 6,
            floatArrayOf(1.90f, 1.96f, 1.99f, 2.01f, 2.03f, 2.05f, 2.07f, 2.08f, 2.10f, 2.11f, 2.13f),
            floatArrayOf(0.05f, 0.1f, 0.2f, 0.5f), 0,
        ),
        // NiMH. Flat working plateau around 1.25 V with a sharp terminal knee, so
        // the estimate is coarse mid-run and sharpens near the end.
        Chem(
            "NiMH", "NiMH", 1.2f, 1.4f, 1.0f, 40, 0,
            floatArrayOf(1.00f, 1.15f, 1.20f, 1.22f, 1.24f, 1.25f, 1.26f, 1.27f, 1.28f, 1.30f, 1.40f),
            floatArrayOf(0.1f, 0.2f, 0.5f, 1.0f), 1,
        ),
        // NiCd. Flatter and a little lower than NiMH, with the knee further down —
        // close enough to be confused for it, far enough that the wrong curve puts
        // the charge estimate out by tens of percent across the plateau.
        Chem(
            "NiCd", "NiCd", 1.2f, 1.35f, 0.9f, 40, 0,
            floatArrayOf(0.90f, 1.10f, 1.16f, 1.19f, 1.21f, 1.22f, 1.23f, 1.24f, 1.26f, 1.29f, 1.35f),
            floatArrayOf(0.1f, 0.2f, 0.5f, 1.0f), 1,
        ),
        // Alkaline. A PRIMARY cell — a capacity test consumes it, it is not
        // recharged afterwards. Included because its steady, steeply sloped
        // decline is both the thing people want to measure and the easiest curve
        // to read a charge state from. Rated at a low continuous drain, hence the
        // low presets.
        Chem(
            "Alkaline (primary)", "Alkaline", 1.5f, 1.6f, 0.8f, 36, 0,
            floatArrayOf(0.80f, 1.00f, 1.10f, 1.17f, 1.23f, 1.28f, 1.32f, 1.37f, 1.43f, 1.50f, 1.60f),
            floatArrayOf(0.05f, 0.1f, 0.2f, 0.5f), 0,
        ),
        // Custom: no per-cell model at all, so no curve and no SoC. The capacity
        // test still runs; its time estimate falls back to the rated-capacity one.
        Chem(
            "Custom", "Custom", 0f, 0f, 0f, 0, 0,
            floatArrayOf(0f, 0f, 0f, 0f, 0f, 0f, 0f, 0f, 0f, 0f, 0f),
            floatArrayOf(0.1f, 0.2f, 0.5f, 1.0f), 1,
        ),
    )

    val CHEM_N: Int get() = CHEMS.size

    /** Index of the Custom entry (the one with no curve). */
    val CUSTOM: Int get() = CHEMS.size - 1

    /** Does this chemistry carry a usable voltage curve? Custom does not. */
    fun hasCurve(chem: Int): Boolean =
        chem in CHEMS.indices && CHEMS[chem].maxCells > 0

    /**
     * Is the pack size fixed by the chemistry (lead-acid 12 V), so the cell-count
     * control should not be offered at all?
     */
    fun cellsFixed(chem: Int): Boolean =
        chem in CHEMS.indices && CHEMS[chem].fixedCells > 0

    /**
     * State of charge (0..1) for a RESTED per-cell voltage, or -1 when the
     * chemistry has no curve. Linear interpolation between table points;
     * out-of-range voltages clamp to empty/full rather than extrapolating,
     * because past either end the curve says nothing useful and a pack above
     * "full" is a wrong cell count, not a 130 % charge.
     */
    fun socFromOcv(chem: Int, vPerCell: Float): Float {
        if (!hasCurve(chem)) return -1f
        val t = CHEMS[chem].ocv
        if (vPerCell <= t[0]) return 0f
        if (vPerCell >= t[OCV_N - 1]) return 1f
        for (k in 1 until OCV_N) {
            if (vPerCell <= t[k]) {
                val span = t[k] - t[k - 1]
                val f = if (span > 1e-6f) (vPerCell - t[k - 1]) / span else 0f
                return ((k - 1) + f) / (OCV_N - 1).toFloat()
            }
        }
        return 1f
    }

    /**
     * The C-rate a pack of this chemistry is conventionally rated at — what the
     * app proposes when it is told a capacity and nothing else.
     */
    fun stdCRate(chem: Int): Float {
        if (chem !in CHEMS.indices) return 0.2f
        return CHEMS[chem].cRate[CHEMS[chem].stdRate]
    }

    /**
     * Suggested series cell count for a measured pack voltage — what the setup
     * screen offers when the user has connected a pack and picked a chemistry.
     * Returns 0 when the chemistry has no curve. Rounds to the nearest count
     * whose nominal-to-full window contains the reading, then clamps to the
     * chemistry's 60 V ceiling.
     */
    fun suggestCells(chem: Int, packVoltage: Float): Int {
        if (!hasCurve(chem) || packVoltage <= 0f) return 0
        val c = CHEMS[chem]
        if (c.fixedCells > 0) return c.fixedCells
        // Divide by a mid-scale per-cell voltage rather than nominal or full: a
        // pack presented for a capacity test is usually charged but not
        // necessarily full, and the midpoint puts the rounding boundary where a
        // guess is least likely to land one cell out.
        val mid = (c.nom + c.full) / 2f
        if (mid <= 0f) return 0
        return Math.round(packVoltage / mid).coerceIn(1, c.maxCells)
    }
}
