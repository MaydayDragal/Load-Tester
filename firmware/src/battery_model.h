// Battery chemistry model — per-cell open-circuit voltage curves and the
// standard test C-rates that go with them.
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
//     lead-acid is rated at the C20 = 0.05C hour rate). Given the pack's size
//     the controller can work the current out instead of the user doing it.
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
  const char *name;
  float nom, full, cut;   // per cell: nominal, fully charged, discharge cutoff
  int maxCells;           // series cells this firmware allows; 0 = no cell model
  float ocv[OCV_N];       // rested per-cell OCV, empty -> full. MUST be ascending.
  float cRate[CRATE_N];   // selectable test rates
  int stdRate;            // index of the rate the chemistry is conventionally rated at
};

// `inline constexpr` rather than `static const`: at namespace scope in a header,
// `static` would give every translation unit that includes this its own copy of
// the tables (and an unused-variable warning wherever they went untouched).
inline constexpr Chem CHEMS[] = {
    // Li-ion (NMC/LCO). Sloped curve throughout — the easiest chemistry to read
    // a state of charge from, and the one where a voltage-based estimate is
    // genuinely trustworthy.
    {"Li-ion", 3.7f, 4.2f, 3.0f, 14,
     {3.00f, 3.45f, 3.57f, 3.63f, 3.68f, 3.73f, 3.79f, 3.87f, 3.95f, 4.06f, 4.20f},
     {0.1f, 0.2f, 0.5f, 1.0f}, 1},

    // LiFePO4. The plateau between ~20 % and ~90 % spans about 150 mV, so
    // voltage says very little about charge state across most of the run. The
    // estimate leans hard on the learned-capacity term here, and the SoC figure
    // is worth treating as a rough indication until the knee arrives.
    {"LiFePO4", 3.2f, 3.65f, 2.5f, 16,
     {2.50f, 3.00f, 3.15f, 3.22f, 3.25f, 3.27f, 3.29f, 3.30f, 3.31f, 3.33f, 3.45f},
     {0.1f, 0.2f, 0.5f, 1.0f}, 1},

    // Lead-acid, per 2 V cell. Near-linear OCV vs charge state, which makes it
    // the textbook case for this method. Note ocv[0] is 1.90 V and not the
    // 1.75 V cutoff: 1.75 is the LOADED end-of-discharge voltage, while a rested
    // flat cell sits near 1.90. Rated at the C20 (0.05C) hour rate by
    // convention, so that is the default here.
    {"Lead-acid", 2.0f, 2.13f, 1.75f, 24,
     {1.90f, 1.96f, 1.99f, 2.01f, 2.03f, 2.05f, 2.07f, 2.08f, 2.10f, 2.11f, 2.13f},
     {0.05f, 0.1f, 0.2f, 0.5f}, 0},

    // NiMH. Flat working plateau around 1.25 V with a sharp terminal knee, so
    // the estimate is coarse mid-run and sharpens near the end.
    {"NiMH", 1.2f, 1.4f, 1.0f, 40,
     {1.00f, 1.15f, 1.20f, 1.22f, 1.24f, 1.25f, 1.26f, 1.27f, 1.28f, 1.30f, 1.40f},
     {0.1f, 0.2f, 0.5f, 1.0f}, 1},

    // Custom: no per-cell model at all, so no curve and no SoC. The capacity
    // test still runs; its time estimate falls back to the rated-capacity one.
    {"Custom", 0, 0, 0, 0,
     {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {0.1f, 0.2f, 0.5f, 1.0f}, 1},
};

inline constexpr int CHEM_N = (int)(sizeof(CHEMS) / sizeof(CHEMS[0]));

// Does this chemistry carry a usable voltage curve? Custom does not.
inline bool hasCurve(int chem) {
  return chem >= 0 && chem < CHEM_N && CHEMS[chem].maxCells > 0;
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
