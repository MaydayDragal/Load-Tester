// Battery chemistry model — per-cell open-circuit voltage curves, pack voltage
// limits, and the standard test C-rates that go with them.
//
// Two things depend on this, and both used to be guesses:
//
//  1. TIME REMAINING. The old estimate was pure coulomb counting against the
//     nameplate rating — "(rated - drawn) / current" — which silently assumes
//     the pack started full and that the test ends when the rating is reached.
//     Neither is true: a half-charged pack showed roughly double the real time,
//     and the test actually ends at the CUTOFF VOLTAGE, which on a high cutoff
//     can be nowhere near empty. Reading state of charge off the chemistry's
//     voltage curve fixes both, and needs no rating at all.
//
//  2. DISCHARGE CURRENT. Capacity is only meaningful at a stated rate, and every
//     chemistry has a conventional one (IEC rates Li-ion and NiMH at 0.2C;
//     lead-acid is rated at the C20 = 0.05C hour rate; alkaline cells are rated
//     at a low continuous drain). Given the pack's size the controller can work
//     the current out instead of the user doing it.
//
// The curves are RESTED open-circuit voltages, so a reading taken under load has
// to be referred back through the pack's internal resistance before it can be
// looked up — see CapacityTest::updateModel(), which measures that resistance
// from the switch-on sag rather than assuming a value.
//
// Accuracy, honestly: these are representative curves for each family, not for
// your specific cell. They are good enough for a time estimate that improves as
// the run proceeds (the engine learns the pack's real capacity from Ah-drawn per
// unit of SoC travelled and stops relying on the curve's absolute calibration),
// and they are NOT good enough to report as a state-of-charge measurement. That
// is why every SoC shown in the UI is prefixed "~".
//
// Every maxCells below is chosen so a FULLY CHARGED pack of that size stays
// inside the EL15's 60 V input rating — the engine refuses anything above it, so
// offering a cell count that cannot be tested would be a trap.
//
// Header-only and pure, like el15_protocol.h: no Arduino, no state, no I/O.
#pragma once

namespace battmodel {

// OCV samples per curve, evenly spaced over state of charge: index k is
// k/(OCV_N-1) of full, so index 0 is empty and index 10 is full. Even spacing
// keeps the lookup a single divide and the table readable as a column.
static const int OCV_N = 11;

// Offered test C-rates per chemistry, ascending.
static const int CRATE_N = 4;

struct Chem {
  const char *name;       // full name, for the detail line
  const char *shortName;  // chip label — keep to ~8 characters
  float nom, full, cut;   // per cell: nominal, fully charged, discharge cutoff
  int maxCells;           // largest series count that stays under 60 V; 0 = no cell model
  int fixedCells;         // non-zero: the pack size is fixed, not user-selectable
  float ocv[OCV_N];       // rested per-cell OCV, empty -> full. MUST be ascending.
  float cRate[CRATE_N];   // selectable test rates
  int stdRate;            // index of the rate the chemistry is conventionally rated at
};

// `inline constexpr` rather than `static const`: at namespace scope in a header,
// `static` would give every translation unit that includes this its own copy of
// the tables (and an unused-variable warning wherever they went untouched).
inline constexpr Chem CHEMS[] = {
    // Li-ion (NMC / LCO / most 18650s). Sloped curve throughout — the easiest
    // chemistry to read a state of charge from, and the one where a
    // voltage-based estimate is genuinely trustworthy.
    {"Li-ion (NMC/LCO)", "Li-ion", 3.7f, 4.2f, 3.0f, 14, 0,
     {3.00f, 3.45f, 3.57f, 3.63f, 3.68f, 3.73f, 3.79f, 3.87f, 3.95f, 4.06f, 4.20f},
     {0.1f, 0.2f, 0.5f, 1.0f}, 1},

    // High-voltage LiPo (LiHV), charged to 4.35 V — common in RC packs. Same
    // shape as Li-ion with the top of the curve stretched; using the plain
    // Li-ion curve on one of these would read a full pack as over 100 % and
    // clamp, losing the whole top of the discharge.
    {"LiPo HV (4.35 V)", "LiHV", 3.8f, 4.35f, 3.0f, 13, 0,
     {3.00f, 3.45f, 3.57f, 3.64f, 3.70f, 3.76f, 3.83f, 3.91f, 4.01f, 4.15f, 4.35f},
     {0.1f, 0.2f, 0.5f, 1.0f}, 1},

    // LiFePO4. The plateau between ~20 % and ~90 % spans about 150 mV, so
    // voltage says very little about charge state across most of the run. The
    // estimate leans hard on the learned-capacity term here, and the SoC figure
    // is worth treating as a rough indication until the knee arrives.
    {"LiFePO4", "LiFePO4", 3.2f, 3.65f, 2.5f, 16, 0,
     {2.50f, 3.00f, 3.15f, 3.22f, 3.25f, 3.27f, 3.29f, 3.30f, 3.31f, 3.33f, 3.45f},
     {0.1f, 0.2f, 0.5f, 1.0f}, 1},

    // Lithium titanate. Low cell voltage, very flat, extremely tolerant of high
    // rates — hence the higher C-rate presets.
    {"LTO (titanate)", "LTO", 2.4f, 2.8f, 1.8f, 21, 0,
     {1.80f, 2.10f, 2.20f, 2.25f, 2.28f, 2.31f, 2.34f, 2.38f, 2.44f, 2.52f, 2.80f},
     {0.2f, 0.5f, 1.0f, 2.0f}, 1},

    // Sodium-ion. Wide, strongly sloped working range (roughly 1.5-4.0 V), which
    // makes it one of the better chemistries for a voltage-based charge state.
    {"Sodium-ion", "Na-ion", 3.1f, 4.0f, 1.5f, 14, 0,
     {1.50f, 2.30f, 2.70f, 2.90f, 3.05f, 3.18f, 3.30f, 3.45f, 3.60f, 3.78f, 4.00f},
     {0.1f, 0.2f, 0.5f, 1.0f}, 1},

    // Lead-acid, FIXED at 12 V (six 2 V cells) — the only size worth offering
    // here, so the cell-count control disappears for it and the cutoff lands on
    // the standard 10.5 V straight away. Near-linear OCV vs charge state, which
    // makes it the textbook case for this method. Note ocv[0] is 1.90 V/cell and
    // not the 1.75 V cutoff: 1.75 is the LOADED end-of-discharge voltage, while
    // a rested flat cell sits near 1.90. Rated at the C20 (0.05C) hour rate by
    // convention, so that is the default.
    {"Lead-acid 12 V", "Pb 12V", 2.0f, 2.13f, 1.75f, 6, 6,
     {1.90f, 1.96f, 1.99f, 2.01f, 2.03f, 2.05f, 2.07f, 2.08f, 2.10f, 2.11f, 2.13f},
     {0.05f, 0.1f, 0.2f, 0.5f}, 0},

    // NiMH. Flat working plateau around 1.25 V with a sharp terminal knee, so
    // the estimate is coarse mid-run and sharpens near the end.
    {"NiMH", "NiMH", 1.2f, 1.4f, 1.0f, 40, 0,
     {1.00f, 1.15f, 1.20f, 1.22f, 1.24f, 1.25f, 1.26f, 1.27f, 1.28f, 1.30f, 1.40f},
     {0.1f, 0.2f, 0.5f, 1.0f}, 1},

    // NiCd. Flatter and a little lower than NiMH, with the knee further down —
    // close enough to be confused for it, far enough that the wrong curve puts
    // the charge estimate out by tens of percent across the plateau.
    {"NiCd", "NiCd", 1.2f, 1.35f, 0.9f, 40, 0,
     {0.90f, 1.10f, 1.16f, 1.19f, 1.21f, 1.22f, 1.23f, 1.24f, 1.26f, 1.29f, 1.35f},
     {0.1f, 0.2f, 0.5f, 1.0f}, 1},

    // Alkaline. A PRIMARY cell — a capacity test consumes it, it is not
    // recharged afterwards. Included because its steady, steeply sloped decline
    // is both the thing people want to measure and the easiest curve to read a
    // charge state from. Rated at a low continuous drain, hence the low presets.
    {"Alkaline (primary)", "Alkaline", 1.5f, 1.6f, 0.8f, 36, 0,
     {0.80f, 1.00f, 1.10f, 1.17f, 1.23f, 1.28f, 1.32f, 1.37f, 1.43f, 1.50f, 1.60f},
     {0.05f, 0.1f, 0.2f, 0.5f}, 0},

    // Custom: no per-cell model at all, so no curve and no SoC. The capacity
    // test still runs; its time estimate falls back to the rated-capacity one.
    {"Custom", "Custom", 0, 0, 0, 0, 0,
     {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {0.1f, 0.2f, 0.5f, 1.0f}, 1},
};

inline constexpr int CHEM_N = (int)(sizeof(CHEMS) / sizeof(CHEMS[0]));

// Does this chemistry carry a usable voltage curve? Custom does not.
inline bool hasCurve(int chem) {
  return chem >= 0 && chem < CHEM_N && CHEMS[chem].maxCells > 0;
}

// Is the pack size fixed by the chemistry (lead-acid 12 V), so the cell-count
// control should not be offered at all?
inline bool cellsFixed(int chem) {
  return chem >= 0 && chem < CHEM_N && CHEMS[chem].fixedCells > 0;
}

// State of charge (0..1) for a RESTED per-cell voltage, or -1 when the chemistry
// has no curve. Linear interpolation between table points; out-of-range voltages
// clamp to empty/full rather than extrapolating, because past either end the
// curve says nothing useful and a pack above "full" is a wrong cell count, not a
// 130 % charge.
inline float socFromOcv(int chem, float vPerCell) {
  if (!hasCurve(chem)) return -1.0f;
  const float *t = CHEMS[chem].ocv;
  if (vPerCell <= t[0]) return 0.0f;
  if (vPerCell >= t[OCV_N - 1]) return 1.0f;
  for (int k = 1; k < OCV_N; k++) {
    if (vPerCell <= t[k]) {
      float span = t[k] - t[k - 1];
      float f = span > 1e-6f ? (vPerCell - t[k - 1]) / span : 0.0f;
      return ((float)(k - 1) + f) / (float)(OCV_N - 1);
    }
  }
  return 1.0f;
}

// The C-rate a pack of this chemistry is conventionally rated at — what the
// controller proposes when it is told a capacity and nothing else.
inline float stdCRate(int chem) {
  if (chem < 0 || chem >= CHEM_N) return 0.2f;
  return CHEMS[chem].cRate[CHEMS[chem].stdRate];
}

}  // namespace battmodel
