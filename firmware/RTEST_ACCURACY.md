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

> **Extended device-wide, 2026-08-01.** Probe wiring is no longer an R-test
> setting: **Settings ▸ Probe wiring** holds it for the whole controller. The
> same physics applies to every mode, not just a sweep — `V_dut = V_terminals +
> I·R_lead` — so with a lead figure entered, `main.cpp compensateProbe()` adds
> that drop back on every status packet before it reaches the UI or the capacity
> engine. The gain is largest where it was previously invisible: a battery cutoff
> was firing at the *load's terminals*, so a 2-wire rig stopped a discharge early
> and under-reported the pack by the lead drop.
>
> The R-test is deliberately **excluded** from that correction and keeps fitting
> the raw packet: it already removes the tare from its own slope (§5 item 4), and
> pre-correcting the volts would subtract the same resistance twice — a
> low-milliohm result could land at zero. Two paths, one tare, applied once each.

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

## 7. The ~1.2 A current-reading glitch (2026-08-06)

A blip shows up in every sweep's current trace, once on the ascent and once on
the descent. It is **the load's own current reading**, not current the circuit
drew, and not anything the controller does.

### What the data says

From `RTEST_003` (Android app, 0.05 → 4 A over 15 s, 250 logged packets, a
~14.3 V source through 0.346 Ω):

| elapsed | commanded | reported I | reported V | V implies I |
|---|---|---|---|---|
| 2.458 s | 1.241 A | **1.7939 A** | 13.9665 V | 1.08 A |
| 2.502 s | 1.305 A | **1.7939 A** | 13.9665 V | 1.08 A |
| 12.894 s | 1.125 A | **0.6226 A** | 13.9506 V | 1.12 A |

Both events sit where the current crosses **~1.2 A**, one per ramp direction. A
60 s sweep the same morning put them at sample 145 and sample 819 of 962 —
again ~1.2 A each way, symmetric about the turnaround.

Three things make it a measurement artifact rather than a real excursion:

1. **The voltage does not move.** A genuine 0.55 A excursion drags a 0.346 Ω
   source down 0.19 V. The terminal voltage instead rose 0.03 V and stayed on
   the fitted line — only the current channel is wrong.
2. **One event is exactly half the true current** (0.6226 A against 1.2452 A).
   In IEEE-754 that is a single bit of the exponent, which is what a scaling or
   range change looks like; noise does not halve a number.
3. **The frames pass the device's own sum-to-zero checksum**, so the load built
   them that way — nothing in the BLE path corrupted them.

The duplicate at 2.458/2.502 s is just the poll running faster than the device's
refresh, so the bad frame is served twice.

### Confirmed on the bench, from a PC (2026-08-06)

The above was inference from app-side logs. It was then tested directly by
driving the load over BLE from a laptop — no phone, no app — with
`tools/el15_bench`, which keeps the raw 28 bytes of every frame. Five profiles,
~3 500 conducting samples. Findings, in order of how much they pin down:

1. **It reproduces with a completely different controller.** The app's own
   0.05→4→0.05 A sweep, driven from the PC, glitched twice — once each way,
   both at ~1.2 A. Nothing on the phone is involved.

2. **Every event happens in a 32 mA window.** Across all captures, 11 spikes,
   and the true current at every one of them lies between **1.175 A and
   1.207 A**. Nothing anywhere else in 0.02–4.0 A.

3. **It needs the current to be MOVING through that window.** 45 s held at
   1.15 A, 1.25 A and 1.20 A — 462 samples, including 15 s sitting *on* the
   threshold — produced **zero** events. So it is not a hunting comparator; it
   fires on the crossing.

4. **About one crossing in three.** 24 crossings of 1.10↔1.30 A produced 8
   events. A slow 140 s full-range ramp crossed twice and produced none, which
   is why a single sweep can look clean.

5. **Only the current field.** The same spike test run over the voltage series
   of every capture: **0 outliers in ~3 500 samples**. Whatever goes wrong,
   goes wrong after the two channels have parted company.

6. **There is no second threshold.** A slow 0.05→5 A scan and 20 crossings of
   0.08↔0.20 A both produced nothing. The low end of an R-test sweep is clean.

7. **The corruption is a one-bit shift of the significand against the
   exponent.** With the true value known from the neighbours, each reported
   value is one of:

   | | reported | at 1.20 A | seen |
   |---|---|---|---|
   | exponent LSB dropped | `v/2` | 0.600 A | down-crossings |
   | significand shifted left 1 | `2v-1` | 1.400 A | either |
   | significand shifted left 2 | `4v-3` | 1.800 A | up-crossings |

   **Correction to an earlier version of this section**, which called these
   exact and universal on the strength of the first handful of events. On 166
   events they are neither:

   - They describe *most* events, not all: **~11 % match none of the three**.
   - They are not literal shifts of the FINAL float's mantissa. A left shift by
     two would leave the bottom two mantissa bits zero, and 13 events that
     invert cleanly as `4v-3` have **no** trailing zeros at all. So the shift
     happens upstream of the float — in a fixed-point intermediate that is then
     converted normally — and the bit pattern of the result carries no
     fingerprint of it. Tested as a mode-decider on the held-out set, trailing
     zeros agree with the truth 14 times and disagree 13: a coin toss.

   What survives is the numeric relationship, which is what the repair below
   uses. A wrong scale factor would still give a clean ×2 or ×4; `4v-3` is what
   you get when a significand moves and its exponent does not.

So: a current-range change at 1.20 A, where the rescale between ranges is done
by shifting the float's fields, and on the crossing sample the shift lands in
the wrong field or by the wrong amount. It is a firmware fault inside the load.
Nothing the controller sends changes it, and nothing the controller does can
prevent it — only refuse the sample.

### What it costs

Three samples out of 250:

| | R | σ_R | R² |
|---|---|---|---|
| every packet fitted | 0.346254 Ω | 1.39 mΩ | 0.99601 |
| the three excluded | 0.346602 Ω | 0.53 mΩ | 0.99942 |

So the artifact barely moves R (0.35 mΩ) but makes the sweep **report itself
2.6× less certain than it measured** — and "reliable" is gated on exactly that
number (§2.2).

### What was done about it

**The Android app** rejects a reading whose current sits further from the
commanded setpoint than `max(0.25 A, 20 %, 4 ramp steps)` — see
`ResistanceTest.offTargetLimit`. Rejected frames are counted, reported
(`off_target_samples` in the CSV, a row in the PDF and the result screen) and
still written to the datapoint log, so the raw sweep remains auditable.

Judged against the bench captures the gate holds up, with one known limit:

- **0 false rejections** in ~3 500 samples across all five profiles. The worst
  honest regulation lag measured anywhere was 0.183 A, against a floor of
  0.25 A — a 1.37× margin, narrower than the 2× the phone data alone suggested,
  which is why the ramp-step term matters: it is what widens the window on a
  fast sweep instead of leaving that margin to be eaten.
- **8 of 11 corrupted samples caught.** The three that slip through are the mild
  `2v-1` mode, whose error at 1.20 A is only 0.2 A — under the floor.

### Catching the ones the command test cannot see

The `|I − target|` window has to be wide — 0.25 A — because the commanded
current leads the measured one by design. The load's mildest corruption at
1.20 A moves the reading only 0.20 A, so it fits inside that window and reaches
the fit as data. Measured over 12 real R-test sweeps (3 698 conducting samples,
15 one-sample corruptions): **the command test alone misses 4 of the 15**, one
of them a textbook `2v-1` — 1.4024 A reported where the ramp was at 1.1967.

A second test closes it, and needs nothing from the command. A ramp is
monotonic, so an honest reading lies **between** the readings either side of it
— and keeps doing so no matter how far the command has run ahead. Lag preserves
that ordering; the load's stale repeats are *equal* to a neighbour, so they
preserve it too. Corruption does not. Over 3 511 samples:

| how far a reading sticks out past BOTH neighbours | |
|---|---|
| median | 0.0000 A |
| 99th percentile | 0.0002 A |
| corrupted readings | ~0.55 A |

Three orders of magnitude of daylight, so the 0.05 A trigger is nowhere near
either edge — anything from 0.03 to 0.08 A flags the same 14–17 samples. Adding
it to the engine (every reading is now held one packet, so both neighbours are
in hand) caught 3 more corruptions across those sweeps: one recovered, two kept
out of the fit, and **no honest sample dragged in**. The ramp's own turning
point is the one place a genuine reading does stand above both neighbours, so
the test stands down within three setpoint steps of the apex and the ends.

### Recovering the reading instead of dropping it

The corruption is invertible — `2r`, `(r+1)/2`, `(r+3)/4` — so a dropped sample
is recoverable IF you can tell which one happened. Nothing in the frame says
(see the correction above), so the engine now holds a suspect reading **one
packet**, interpolates the ramp across it from the good samples either side, and
takes the candidate nearest that line — accepting it only within **0.03 A** and
with no rival within 2.5×. Otherwise it is dropped and counted, as before.

Five discriminators were tried against the bench data before that one. The
tolerance and the interpolation are both load-bearing:

| rule | right | **wrong** | dropped |
|---|---|---|---|
| nearest to the commanded setpoint | 2 | **9** | 68 |
| nearest to extrapolated past samples | 56 | **4** | 19 |
| both of those must agree | 2 | **2** | 75 |
| trailing-zero bit fingerprint | — | coin toss | — |
| **interpolate across it, tol 0.03 A** | **104** | **0** | 62 |

(the first three over one 79-event run; the last over 166 events across two
independent 200-crossing runs at the sweep's own slew rate). At a 0.06 A
tolerance the same rule fabricates 8 currents, which is why 0.03 is not a
comfort margin. Replaying the whole engine over all nine captures — ~9 000
samples — repairs 116, drops 75, and gets **none** wrong; the negative controls
(steady hold, full-range scan, low-current cycle) repair nothing at all.

**It does not improve the measurement, and is not meant to.** On RTEST_003:

| | n | R | σ_R | R² |
|---|---|---|---|---|
| no gate | 250 | 0.346254 | 1.392 mΩ | 0.99601 |
| gate only | 247 | 0.346602 | 0.532 mΩ | 0.99942 |
| gate + repair | 248 | 0.346697 | 0.549 mΩ | 0.99938 |

A reconstruction carries ~12 mA of error, so it is slightly noisier than a real
sample; a sweep crosses 1.20 A only twice, so at most about one sample per run
comes back. Both differences sit far under the 0.6 mΩ run-to-run repeatability
of §6 — this recovers data without fabricating any, and that is the whole claim.
The `repaired_samples` count is in the CSV, the PDF and the result screen so a
report never leans on reconstructions silently.

**The firmware deliberately does not have this gate yet.** It is the same class
of change reverted on 2026-08-06 for being unproven, and it stays unproven on
the board until a sweep is run with it. Until then the two engines differ in
this one respect, and a board-saved report will show the wider σ_R.

---
*Companion: resistance_test.h (engine), CAPACITY_PLAN.md, FEATURE_IDEAS.md.*
