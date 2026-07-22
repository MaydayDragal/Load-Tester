# R-Test Measurement Accuracy — Evaluation & Better Methods

Evaluation of the circuit-resistance test in `firmware/src/resistance_test.h`
(fit math) + `firmware/src/ui.cpp` result rows. Focus: where measurement error
comes from, and what actually improves accuracy on this hardware.

## 1. What the current method does

1. **Prime** (load off): measure open-circuit voltage `Voc` — keeps the *last*
   priming packet's voltage.
2. **Ladder**: build `n` current setpoints, evenly spaced `maxI·k/n` for
   `k = 1..n` (does not include 0). `maxI = min(0.8·fuse, 12 A, 150 W/Voc, 40 A)`.
3. **Per step**: settle 800 ms (discarded), then average all samples in a
   1500 ms collect window → one `(I, V)` point.
4. **Fit**: ordinary least squares `V = Voc − R·I`; `R = −slope`. Report `R`,
   intercept as Voc, and **R²** as the confidence indicator.

This is sound in structure — the slope method is the right idea, and it has one
real strength (see §3.1). The accuracy limits are in the *details*.

## 2. Where accuracy is actually lost (ranked by impact)

### 2.1 Monotonic ramp couples time-drift into the slope — **biggest systematic error**
Current rises 0→max monotonically over ≈ `n·(0.8+1.5)` s (≈ 18 s at 8 steps).
Over that time the DUT drifts *in the same direction as current*:
- wire/contact **I²R self-heating** raises copper resistance (~+0.4 %/°C),
- a **battery sags** (state-of-charge drop + polarization build-up).

Because the drift is monotonic in time and current is monotonic in time, the
drift is **correlated with current**, so it adds directly to the slope. The
high-current points sit lower in V than pure IR predicts → **R biased high**
(and Voc slightly off). This is a true systematic bias, not noise — averaging
more does not remove it.

**Fix (no hardware change): bidirectional sequencing.** Sweep the ladder
**up then back down** (0→max→0) and average the two `(I,V)` readings at each
current level. A symmetric sequence makes the first-order (linear-in-time) drift
cancel: the ascending point sees drift `+δ`, the descending point at the same
current sees `−δ` about the midpoint. This is the classic "reverse the order to
cancel linear drift" technique and it is the single highest-value change here.

### 2.2 Confidence is reported as R², which is the wrong statistic
R² answers "how straight is the line," not "how well do I know R." Two failure
modes:
- **Low resistance, good measurement** (e.g. a clean 5 mΩ bus bar): the V change
  is small vs. ADC noise, so R² is low — the UI cries "low confidence" on a
  perfectly valid result.
- **High resistance**: R² is ~1.000 even with sloppy data, because the signal
  dwarfs the noise — false confidence.

**Fix: report the slope's standard error** — an actual ± tolerance on R
(`R ± σ_R`, e.g. `0.348 ± 0.004 Ω`). It falls straight out of the same sums the
fit already computes:
`σ_R = sqrt( (Σdv² − slope·Σdi·dv) / ((n−2)·Σdi²) )`.
Then "reliable" = *relative* tolerance `σ_R/R` below a threshold (e.g. 2–5 %),
which is meaningful at any resistance scale. Keep R² as a secondary linearity
readout.

### 2.3 Too few samples per point
At the 500 ms default poll, a 1500 ms collect window yields only **~3 samples**
per step, so each `(I,V)` point is a 3-sample mean with high variance — and
slope variance scales with per-point variance.

**Fix: exploit the now-adjustable sample rate.** At 10 Hz the same window
captures ~15 samples/step (≈ 4× lower per-point σ, ≈ 2× tighter R). The engine
should either recommend/auto-raise the rate during a sweep, or lengthen the
collect window when the rate is low. Effectively free precision.

### 2.4 Even spacing is robust but not the most precise use of N points
Slope variance ∝ `1/Σ(Iᵢ − Ī)²`. For a *fixed* number of measurements, that sum
is maximised by putting points at the two **extremes** (D-optimal design for a
line), not spreading them evenly. Even spacing trades precision for the ability
to *see* nonlinearity.

**Recommendation:** keep even spacing as the default (its diagnostic value
matters — see §2.5), but bias the ladder slightly toward the endpoints
(e.g. a few extra points at min and max), or expose a "precision vs. linearity"
choice. Secondary to §2.1–2.3.

### 2.5 No curvature / residual check
A single line fit can't distinguish "resistor" from "battery with polarization"
or "connection that behaves nonlinearly." R² near 1 hides gentle curvature.
Cheap addition: check the fit **residuals for structure** (or fit a quadratic
and test the 2nd-order term). A significant curvature term should be surfaced —
it usually means the DUT isn't a simple resistance and the single-R number is
misleading.

### 2.6 Minor
- **Voc uses the last priming packet, not an average** — average the priming
  window for a cleaner Voc (it feeds the power clamp and could anchor the fit).
- **Settle time vs. battery polarization** — 800 ms may be short for a battery
  to settle after a current step; residual settling biases each point downward.
  Bidirectional averaging (§2.1) also mitigates this.

## 3. Things the current method already gets right (don't "fix" these)

### 3.1 The intercept absorbs constant offset EMFs — a real strength
Thermal EMFs (Seebeck at dissimilar-metal junctions) and fixed sensor offsets
would corrupt a naive single-point R = V/I measurement. Because this method
fits a *line* and puts any current-independent offset into the **intercept**,
a constant offset does **not** bias R. This is why the slope method beats
"measure V at one current and divide." Lab instruments use current-reversal to
cancel these; the EL15 is sink-only so it can't reverse current — but the linear
fit already handles the *constant* part, so reversal buys little here.

### 3.2 OLS (not Deming/total-least-squares) is the right choice
OLS assumes negligible error in the x-variable (current). In CC mode the load
*regulates* current, and the ladder spans a wide current range, so
`σ_I²/Var(I)` is tiny and the OLS attenuation bias is negligible. Deming
regression would add complexity for no real gain.

## 4. The hardware ceiling: 2-wire vs. 4-wire

The EL15 senses voltage at **its own terminals**, so the measured V includes the
**test-lead and clip resistance** between the instrument and the DUT. For
milliohm-scale targets this lead/contact resistance can dwarf the DUT and is the
dominant *absolute* error — no firmware change can remove it.

- If the EL15 exposes **remote (sense) terminals**, use 4-wire Kelvin
  connection: the firmware can't do this, but the UI should *guide* it (a
  "4-wire" setup flag + instructions), and a zero/tare step (§4.1) partly
  compensates 2-wire use.
- **Tare/zero (2-wire):** measure the lead+clip resistance with the DUT
  shorted, store it, subtract from the result. Turns a 2-wire rig into a
  usable low-R tool. Cheap firmware feature, high real-world value.

## 5. Recommended change set (no new hardware)

Priority order, all firmware-only:

1. **[IMPLEMENTED]** **Bidirectional (up/down) current sequence + per-level
   averaging** — cancels the dominant time-drift bias (§2.1). The ladder is now
   walked as a triangle over the distinct levels and the two visits per level
   average together.
2. **[IMPLEMENTED]** **Report `R ± σ_R` tolerance; "reliable" on tolerance** —
   the result shows an "Uncertainty (±)" row (green ≤5 mΩ or ≤5 %, else amber);
   R² demoted to a neutral secondary readout (§2.2).
3. **[IMPLEMENTED]** **More samples/step via the sample rate** — the collect
   window now scales to ~10 readings/step (capped 2.5 s), so a higher Settings
   sample rate tightens the fit (§2.3).
4. **Tare/zero step** to subtract lead+contact resistance (§4.1) — biggest
   real-world absolute-accuracy win short of true 4-wire. *(not yet done)*
5. **Curvature/residual flag** to catch non-resistive DUTs (§2.5). *(not yet done)*
6. Minor: average the priming Voc; optional endpoint-weighted ladder. *(not yet done)*

Note: bidirectional roughly doubles the physical step count (n levels → 2n−1
steps) and the adaptive window can lengthen each step, so a sweep takes
noticeably longer than before — the accuracy trade. Reduce the step count or
raise the sample rate to claw the time back.

Expected effect: (1)+(3) tighten and de-bias R by the largest margin; (2)+(5)
make the *reported confidence* trustworthy; (4) fixes absolute accuracy at low
resistance. None require reversing current or new silicon.

---
*Companion: resistance_test.h (engine), CAPACITY_PLAN.md, FEATURE_IDEAS.md.*
