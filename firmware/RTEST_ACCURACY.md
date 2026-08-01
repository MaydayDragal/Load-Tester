# R-Test Measurement Accuracy — Evaluation & Better Methods

Evaluation of the circuit-resistance test in `firmware/src/resistance_test.h`
(fit math) + `firmware/src/ui.cpp` result rows. Focus: where measurement error
comes from, and what actually improves accuracy on this hardware.

> **Superseded by the continuous sweep (2026-08-01).** Sections 1–5 below were
> written against the STEPPED ladder — settle, collect, average, repeat — which
> no longer exists. The engine now ramps current smoothly up and back down over
> a user-set duration and fits every status packet it receives. The analysis
> still holds; what changed is how the points are gathered. See
> [§6](#6-the-continuous-sweep-2026-08-01) for what carried over and what the
> ramp changes.

## 1. What the stepped method did (historical)

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
4. **[IMPLEMENTED]** **Tare/zero step + 4-wire support** (§4) — R-Test setup has
   a **2-wire / 4-wire (Kelvin)** toggle with hook-up guidance. In 2-wire,
   *"Measure (short the probes)"* runs a full sweep with the probes shorted and
   stores the raw slope in NVS as `tareOhm`; every later 2-wire result subtracts
   it (clamped at zero) and still reports the uncorrected figure as
   *"Measured (incl. leads)"*. In 4-wire nothing is subtracted — the sense leads
   carry no current, so there is nothing to correct. The wiring and the tare are
   both carried into `RTEST_NNN.CSV`. Note the reliability test judges σ against
   the **raw** slope, not the tare-corrected one: subtracting a constant cannot
   improve a fit, and a near-tare result would otherwise look wildly unreliable.
5. **Curvature/residual flag** to catch non-resistive DUTs (§2.5). *(not yet done)*
6. Minor: average the priming Voc; optional endpoint-weighted ladder. *(not yet done)*

**Sample-density note (updates §2.3).** That section was written when the poll
default was 500 ms and assumed a 10 Hz ceiling. Real hardware turned out to cap
at **~17–19 fresh samples/s**, and the default poll is now **50 ms (20 Hz)**, so
the adaptive collect window reaches its ~10-readings-per-step target inside the
2.5 s cap at the default rate. Polling faster than 20 Hz buys nothing — it
refetches repeated frames.

**Still unproven with real current.** Every technique above has been exercised
against the phone simulator and verified by reading the code; no R-test has yet
been run with actual current flowing through a real EL15. Until that happens,
treat the accuracy claims here as design intent rather than measured results.

Note: bidirectional roughly doubles the physical step count (n levels → 2n−1
steps) and the adaptive window can lengthen each step, so a sweep takes
noticeably longer than before — the accuracy trade. Reduce the step count or
raise the sample rate to claw the time back.

Expected effect: (1)+(3) tighten and de-bias R by the largest margin; (2)+(5)
make the *reported confidence* trustworthy; (4) fixes absolute accuracy at low
resistance. None require reversing current or new silicon.

---

## 6. The continuous sweep (2026-08-01)

The stepped ladder was replaced by a **continuous triangular current ramp**:
`start -> max -> start` over a user-set duration, with every status packet fed
into the fit.

### What carried over unchanged

- **§2.1 drift cancellation.** The ramp is symmetric in time, so every current
  is visited once going up and once coming down, equally spaced about the sweep
  midpoint. First-order drift still cancels — this is the same argument as the
  up-then-down ladder, applied continuously.
- **§2.2 uncertainty as the confidence statistic.** Still `R ± σ_R`, still
  gating "reliable" on absolute (≤5 mΩ) or relative (≤5 %) tolerance, still
  judged against the *raw* slope rather than the tare-corrected figure.
- **§3.1 the intercept absorbing constant offsets**, and **§3.2 OLS over
  Deming** — both are properties of fitting a line, not of how it is sampled.
- **§4 the 2-wire ceiling** and the tare that partly compensates it.

### What the ramp changes

- **§2.3 sample density is no longer the limiting factor.** Every packet counts
  instead of ~10 per level, so a 30 s sweep collects ~300 points where the
  8-step ladder collected ~80 in about the same time. The fit runs incrementally
  on six running sums, so this costs no memory and makes a **live** R estimate
  free — which is what the running screen now plots.
- **§2.4 point placement is now uniform in time**, hence uniform in current.
  The D-optimal argument for weighting the endpoints still stands and is still
  not implemented; a dwell at each extreme would be the way to get it.
- **§2.6 settle time no longer exists as a parameter.** Nothing waits for the
  load to settle, because nothing needs a settled reading: V and I inside one
  status frame are simultaneous, so regulation lag shifts *which* current a
  packet reports, not the voltage that belongs with it. What lag does cost is
  span — the measured current trails the commanded ramp slightly at the turning
  point — which is why the result reports the current range it actually
  measured rather than the one it commanded.
- **New caveat: correlated noise.** With ~300 samples the reported σ_R is roughly
  √(300/80) ≈ 2× tighter than the ladder's for the same data quality. That is
  only honest if sample errors are independent. They largely are (the EL15
  re-samples per frame), but a slow systematic — a thermal drift the symmetric
  ramp does not fully cancel — will not show up in σ_R. Treat a very small σ_R
  on a very short sweep with the same suspicion as any other overconfident
  statistic.
- **Reported curve.** Raw samples feed the fit; the result curve and the CSV
  carry them **binned into 32 current bands**, each band averaging its up-ramp
  and down-ramp visits. That keeps the V-I chart readable and the CSV compact
  without the engine allocating per sample.

### Measured on real hardware (2026-08-01)

First numbers from an actual EL15 rather than reasoning. DUT was a bench source
of ~20 V through test leads; absolute R is not the point, repeatability is.

**0.5 A / 10 s / 20 Hz, three repeats:** R = 84.15 mΩ, **run-to-run spread
0.61 mΩ (0.72 %)**, R² 0.995–0.998, ~123 samples, ΔT < 1.1 °C.

**Is σ_R honest?** This is the question §2.2 rests on, since "reliable" is gated
on it. Measured both ways:

| Sweep | mean σ_R | true run-to-run spread | verdict |
|---|---|---|---|
| 0.3 A / 10 s | 0.81 mΩ | 0.67 mΩ | slightly conservative |
| 0.5 A / 10 s | 0.45 mΩ | 0.61 mΩ | slightly optimistic |

So σ_R is the right order of magnitude — within ~1.4× either way — which is what
the reliability gate actually needs. It is **not** a conservative bound; do not
read it as one.

**Sample rate vs. quality.** A three-way A/B of the control-write-to-poll gap
(HANDOVER §10) found σ_R *flat* at 0.82–0.97 mΩ while the sample count ranged
from 87 to 150. More samples came with proportionally noisier individual points
(R² 0.979 at 150 samples vs 0.987 at 90), and the two effects cancelled almost
exactly. **Sample count is not the limiting factor at these currents** — which
retro-justifies §2.3 being demoted: the fit is limited by something systematic,
not by counting statistics.

**What that implies for §2.1.** If white noise is not the binding constraint, the
residual error is systematic — drift, contact resistance, regulation lag — and
the symmetric ramp's drift cancellation is doing the real work. Two observations
support that: the same physical setup read 53.6, then 84.2, then 77.4 mΩ across
one session's reflashes (contact resistance moving between sessions, not within
them), and ΔT stayed under 1.1 °C so self-heating was *not* the dominant term at
0.5 A. At higher currents it would be, and the ramp symmetry matters more.

**Untested:** whether longer sweeps help. The 20 s and 60 s arms were lost when
the bench source collapsed mid-run.

### Still not done (from §5)

- **Curvature/residual flag** (§2.5) — arguably easier now: with 300 points and
  both ramp directions in each band, a systematic up-vs-down difference is a
  direct hysteresis indicator.
- **Averaging the priming Voc** (§2.6) and an **endpoint-weighted ladder** (§2.4).

---
*Companion: resistance_test.h (engine), CAPACITY_PLAN.md, FEATURE_IDEAS.md.*
