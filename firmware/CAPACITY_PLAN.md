# Battery Capacity Test — Overhaul Gameplan

Goal: turn CAP from a bare device mode into a full battery bench test — any pack
size/voltage the EL15 can handle (0.1–60 V, 12 A, 150 W), automatic minimum-
voltage cutoff, continuous V/I/Ah/Wh/temp logging, live discharge curve, and
results savable to SD like the R-test report (which itself becomes real, not a
stub, as part of this work).

---

## 1. What existed when this plan was written (2026-07-21 starting point)

*Historical — everything below is superseded. Jump to §4 for current phase
status.*

- CAP is selectable; the device integrates Ah/Wh itself and reports them in the
  CAP-mode status tail (no temperature in those packets). The UI shows Ah in
  the telemetry bar. No cutoff, no logging, no completion, no safety envelope.
- The R-test engine (`resistance_test.h`) is the architectural template:
  timer-driven state machine pumped from `loop()`, fed by `onStatus()`,
  callbacks into the UI, LOAD_OFF on every exit path.
- "Save to SD" is a stub everywhere — the SD driver is Phase 0 of this plan.
- Hard-won lessons that carry over: no heap churn at test completion
  (persistent rows / fixed-capacity charts), immediate phase feedback on start,
  scroll reset per phase, dirty-checked live labels.

## 2. Feature design

### 2.1 Battery setup (new Setup phase)
- **Chemistry presets** (per-cell nominal / full / cutoff defaults):
  - Li-ion: 3.7 / 4.2 / 3.0 V
  - LiFePO4: 3.2 / 3.65 / 2.5 V
  - Lead-acid (per 2 V cell): 2.0 / 2.13 / 1.75 V
  - NiMH: 1.2 / 1.4 / 1.0 V
  - Custom: keypad-entered cutoff, no per-cell logic
- **Cell count** −/+ (1–20S) with an **auto-suggest**: at setup the live Voc is
  shown and the firmware proposes the S count whose per-cell voltage best fits
  the chosen chemistry (e.g. 12.45 V Li-ion → "3S (4.15 V/cell)"). Wrong-count
  mismatch (per-cell outside plausible window) shows an amber warning.
- **Cutoff voltage** auto-filled = cells × per-cell cutoff, always editable via
  keypad. This is the "automatic min voltage stop point."
- **Discharge current** via keypad/steps, clamped to min(12 A, 150 W ÷ Voc).
  Optional capacity hint (mAh) enables a C-rate helper (0.2C/0.5C/1C chips)
  and an ETA/progress estimate while running. **Done 2026-08-01** — the chips
  carry each chemistry's *own* conventional rates (lead-acid at the C20 hour
  rate, the rest at 0.2C per IEC) and the ETA was rebuilt on the discharge curve
  rather than the rating (§4c).
- **Safety caps**: max duration (default 12 h) and max Ah (default 1.5× the
  hint, or 50 Ah) — belt-and-braces stops.

### 2.2 Test engine (`capacity_test.h`, sibling of ResistanceTest)
- States: IDLE → PRIME (read Voc, sanity: 0.1–60 V, Voc > cutoff + margin,
  chemistry plausibility) → DISCHARGING → REST (optional 60 s open-circuit
  recovery measurement after cutoff) → done.
- **Run in CC mode and integrate Ah/Wh locally** (trapezoidal over sample
  timestamps). Rationale: CC packets carry temperature (CAP packets don't),
  local integration matches what we log, and the engine keeps full control.
  The device's own CAP integration remains available as a cross-check mode if
  we ever want it (decision recorded here; revisit against a real EL15).
- **Cutoff logic with debounce**: stop when 3 consecutive samples ≤ cutoff, or
  immediately when V < cutoff − 0.3 V (noise-proof but fail-safe). Also stop on
  protection trip, max time, max Ah, manual STOP, disconnect.
- Every exit path: LOAD_OFF + setpoint 0 (same discipline as the R-test).
- Result struct: Ah, Wh, duration, start/end/rebound V, avg V, avg I (commanded
  vs measured), min/max temp, max fan, cutoff used, stop reason.

### 2.3 Logging & memory budget
- **Full-rate stream to SD** (Phase 3): one CSV row per status sample. At 1 Hz
  a 5 h test ≈ 18 k rows ≈ 150 kB — trivial for SD, no RAM pressure.
- **In-RAM downsampled curve** for the UI: fixed 720-point buffer (~12 kB for
  t/V/I/Ah). When full, decimate 2× and double the stride — a bounded buffer
  that always spans the whole test. Live chart (V vs time, I on the secondary
  axis) on the Running screen; final curve on the Result screen. Fixed-capacity
  chart series allocated at build (no reallocs — see the 2026-07-21 panic).

### 2.4 SD card (Phase 0 — DONE and hardware-verified 2026-07-24)
- ESP32-C6 has **no SDMMC host** — the slot runs in SPI mode: SCK=11,
  MOSI(CMD)=10, MISO(D0)=18, **CS=6** (confirmed against Waveshare's
  `pin_config.h`, which calls them `SDMMC_*`).
- The C6 also has only **one general-purpose SPI host**, and the AMOLED owns it
  in QSPI mode. The original plan — make the card a second device on that host
  and re-route the signals through the GPIO matrix per access — **was tried and
  does not work**: the IDF `sdspi` driver cannot transact on the panel's bus and
  card init dies at CMD59 (`ESP_ERR_NOT_SUPPORTED`). That scheme is deleted.
- The card now runs entirely on **bit-banged software SPI** on its dedicated
  pins, via SdFat with our own `SdSpiBaseClass` driver (`SPI_DRIVER_SELECT=3`).
  Plain `digitalWrite/digitalRead` gives an inherently slow, reliable ~250 kHz
  clock; SdFat's own `SoftSpiDriver` used fast register GPIO that corrupted
  512-byte block writes. `USE_SD_CRC=1` is required (the card rejects SdFat's
  fixed bogus CRC byte at ACMD41), and the config is `SHARED_SPI`, not
  `DEDICATED` (a dedicated multi-block write was left un-terminated and broke the
  next file open). Nothing touches SPI2, so a card access and a screen redraw are
  now fully independent. Full rationale in `sd_card.cpp` and `HANDOVER.md` §12.
- `sd::saveCsv()` writes from the loop task and **stays mounted** — re-running
  card init over software SPI is flaky and there is no bus to give back.
  Card-absent and write-failure states surface honestly in the UI (no fake
  "Saved"), and a half-written file is deleted rather than left as a "result".
- Verified end-to-end on hardware: mount → two writes with an incrementing
  index → byte-correct readback. A **wedged card** (from an aborted write)
  answers CMD0 with `R1=0x00` and needs a physical reseat — an ESP reset won't
  clear it. The **UI Save button path is still unexercised**.
- Naming: `RTEST_NNN.CSV` / `BATT_NNN.CSV`, next index by directory scan.
  Metadata header rows (config, firmware build, RTC timestamp if set — the CSV
  says "(RTC not set) uptime NNN s" when it is not).
- The R-test save writes a real CSV (per-level samples + summary) — closes
  QA-report finding M5. Remaining for Phase 3: streaming the capacity test's
  per-sample discharge curve (today's `BATT_NNN.CSV` holds the summary only).

### 2.5 UI (new `SCR_BATT` screen, Menu grows to 8 tiles = exactly 4×2)
- **Setup**: chemistry tiles, cells −/+ with Voc/auto-suggest readout, cutoff
  (keypad), current (keypad + C-rate chips when hint set), safety caps row,
  big Start (disabled until valid, shows the computed plan:
  "2.0 A → stop at 9.0 V").
- **Running**: elapsed + ETA, hero V with cutoff marker, I, Ah, Wh, temp,
  live curve, STOP. Phase entry resets scroll; start shows this screen
  immediately (enterRtRun pattern).
- **Result**: big Ah, Wh + duration row, start/end/rebound V, avg rows, temp
  range, full curve, honest Save-to-SD state, New test.
- Mode picker: CAP stays (raw device mode) — the picker gains a "BATT" tile
  (amber, like RT) that routes to the new screen instead of sending a mode.

### 2.6 Persistence & clock (Phase 4 — DONE)
- NVS (`prefs.cpp`): battery + R-test setup, sample rate, brightness, volume/
  mute, screen protection, Wi-Fi credentials, last device, auto-connect. Writes
  are debounced (1.5 s settle) so a slider drag is one flash write; the
  crash-recovery in-flight flag and the Wi-Fi credentials are written
  synchronously on purpose.
- Clock: the manual stepper UI was **replaced by Wi-Fi NTP** (`netclock.cpp`) —
  Settings ▸ Clock scans for networks, takes a password and a UTC offset, and
  "Sync clock now" writes the PCF85063 (clearing its oscillator-stop flag).
  Reports then carry a real timestamp; until the clock is set the CSV says
  `(RTC not set) uptime NNN s`. A manual set-time UI is still a nice-to-have for
  benches with no Wi-Fi. Note a successful sync **auto-reboots** — see
  `HANDOVER.md` §8 for why (the draw buffer cannot be reassembled after the
  low-memory window).

## 3. Safety & failure-mode review (design-time)

| Risk | Mitigation |
|---|---|
| BLE drops mid-discharge — no supervisor for cutoff | **Done** (`link_guard.h`): whenever the device reports the load on, the guard is armed; a drop from a live link starts up to 8 reconnect-and-force-LOAD-OFF attempts with a red banner + repeating alarm, and gives up loudly with a tappable retry. Residual: the guard needs a working radio, so the device keeps sinking until its own UVP if the link never comes back — **set the EL15's hardware UVP as a backstop when testing real packs**. Untested against a real drop. |
| ESP reboot mid-test | **Done**: NVS in-flight flag written *synchronously* while energised; on boot the amber "restarted with the load ON" banner offers reconnect-and-force-off. Untested for real. |
| Controller's own battery dies mid-test | **Done** (`main.cpp monitorPower()`): on battery (not USB), ≤ 8 % or ≤ 3.30 V for 3 consecutive 1 Hz reads force-stops the load before the controller can brown out and strand it. Keep the controller on USB for long unattended runs. |
| Power-off strands an energised load | **Done**: a long PWR press forces LOAD OFF and flushes it over BLE *before* `display::powerOff()` cuts the rails, instead of the PMIC's own OFFLEVEL cutoff. |
| Wrong cell count / cutoff too low | Auto-suggest + plausibility warning at setup; hard floor 0.1 V. |
| Noise triggers early cutoff | 3-sample debounce; single-sample only below cutoff − 0.3 V. |
| SD removed / full mid-test | Stream failures flip the UI save state to "SD error — RAM curve retained"; result save can retry. |
| Long-test heap creep | Fixed buffers only; no allocation after start (R-test lesson). |

## 4. Phases & order of work

| Phase | Scope | Status |
|---|---|---|
| 0 | ~~SD bring-up, `sd::` module, real R-test CSV save, CS-pin verification~~ | ✅ **done** — rewritten onto bit-banged software SPI and **verified on hardware 2026-07-24** (§2.4). UI Save buttons still unexercised |
| 1 | ~~`capacity_test.h` engine~~ | ✅ **done**. The on-device simulator was *removed* rather than extended — the battery model lives in the Android simulator app (`simulator/`), so the engine is always tested over a real BLE link |
| 2 | ~~SCR_BATT setup/running/result UI + charts + picker/Menu wiring~~ | ✅ **done**, including the steady discharge curve (fixed time frame, smoothing, stepped auto-zoom Y scale) |
| 3 | Per-sample datapoints in the CSV | ✅ **done 2026-08-01** — but *buffered in flash*, not streamed to the card. `sample_log.{h,cpp}` writes 24-byte records to LittleFS during the run and `report.h` streams them into `BATT_NNN.CSV` at the end. Streaming straight to the card was rejected: every SD write blocks the loop task over a ~250 kHz bit-banged link, so doing it per sample would stall the UI and the BLE poll for the whole run, and a card pulled mid-test would take the log with it |
| 4 | ~~NVS persistence, clock, QA-guide update~~ | ✅ **done** (clock via Wi-Fi NTP rather than a stepper UI, §2.6). A full QA pass with **real current** is still outstanding |

Remaining: the hardware validation all of the above still needs — **no capacity
run has yet drawn real current**.

### Beyond the original plan (landed 2026-08-01)

- **Pause / resume.** A critical controller battery or a dropped BLE link now
  PAUSES the discharge instead of ending it: load off, clock stopped, Ah/Wh and
  the flash log intact, amber banner + RESUME button. The controller-battery
  case auto-resumes when power returns. `durationS` counts active time only and
  the result reports `pausedS` separately. This closes the §3 risk table's worst
  outcome — losing hours of measurement to a transient.
- **Auto-save on completion**, with the manual button demoted to a retry.
- **Rated capacity (optional)** → C-rate, expected runtime, live "% of rated
  drawn" + ETA, and a **state-of-health** figure on the result and in the CSV.
  §2.1's "capacity hint / C-rate helper / ETA" item, now done.
- **Running-test chip** in the status strip so a test can be navigated away from
  and returned to. Fixes the real defect behind it: the BLE handler used to
  treat the user's own scan as a link drop and tore the test down.

## 4a. What the engine actually does today (vs. §2.2)

`capacity_test.h` implements §2.2 as designed, with these concrete values:

- States `IDLE → PRIMING (1.5 s open-circuit) → DISCHARGING → RESTING (60 s) → done`.
- Priming aborts on `Voc < 0.1 V`, `Voc > 60 V`, or `Voc ≤ cutoff + 0.2 V`.
- Discharge current is clamped to `min(request, 12 A, 150 W ÷ Voc)`; below
  0.01 A the test refuses to start.
- Ah/Wh integrate per sample; a gap longer than 36 s (link stall) is discarded
  rather than integrated across.
- Cutoff debounce is exactly as specified: 3 consecutive samples ≤ cutoff, or a
  single sample below `cutoff − 0.3 V`.
- Safety caps: 12 h max duration, 50 Ah max.
- Every exit path runs `finishSafely()` (LOAD OFF + setpoint 0), and every
  `stop()` fires exactly one of `onComplete` / `onError` — a mid-discharge stop
  yields a valid *partial* result rather than discarding the data.
- **Not implemented from §2.1:** the per-chemistry plausibility warning on cell
  count (the count is capped per chemistry, and the Voc line turns amber when the
  per-cell voltage is implausible, but there is no explicit mismatch warning).

## 4b. Test current from pack size — C-rate chips (2026-08-01)

A capacity figure only means something at a stated rate, and every chemistry has
a conventional one. So the setup screen takes the pack's **size** and works the
**current** out, instead of asking for amps and reporting the rate afterwards.

- Four chips per chemistry, from `battmodel::Chem::cRate`: lead-acid
  **0.05 / 0.1 / 0.2 / 0.5C** (it is rated at the C20 = 0.05C hour rate by
  convention), everything else **0.1 / 0.2 / 0.5 / 1C** with 0.2C as the default
  (IEC 61960 for Li-ion, IEC 61951 for NiMH).
- Tapping a chip sets `current = C × ratedAh`, clamped to the load's 12 A /
  150 W envelope; the hint says so explicitly when the clamp, not the chip, is
  what set the current.
- Entering (or changing) the rated capacity re-applies the selected chip, so
  telling the controller the pack size is by itself enough to get a test current.
- Typing a current by hand sets the selection to −1 and the chips go quiet —
  an explicit amps figure must not be silently overwritten by a later rating
  edit. The selection persists in NVS (`btCRIdx`).

## 4c. Time remaining from the discharge curve (2026-08-01)

The old estimate was `(ratedAh − drawn) / current`: pure coulomb counting against
the nameplate. It was wrong in two independent ways — it assumed the pack started
**full**, and it counted down to the **rating** when what actually ends the test
is the **cutoff voltage**. A half-charged pack showed roughly double the truth,
and a deliberately high cutoff showed an ETA long past the point the run stops.

The replacement reads charge state off the chemistry's own voltage curve
(`battery_model.h`, curves per family with a per-cell OCV table):

1. **Internal resistance first.** The load reports a voltage sagged by I·R of the
   pack, its contacts and the leads, so it cannot be looked up on a *rested* OCV
   curve directly — at 2 A through 150 mΩ that is 0.3 V, which on Li-ion is most
   of the difference between 40 % and 70 % charged. Priming already measures the
   open-circuit voltage; the first seconds of discharge give the loaded voltage
   at a known current; R falls out of the two. Averaged over a 1.5–5 s window
   (active time, so a pause cannot burn it) and only over samples where the load
   is actually regulating, with a 3-sample quorum before it is trusted.
2. **Charge state** = `socFromOcv(chem, (V + I·R) / cells)`, filtered with a 30 s
   time-constant EMA — time-based, not per-sample, because the sample rate is a
   user setting spanning 2–20 Hz.
3. **The target is the cutoff, in charge terms.** The engine cuts on the *loaded*
   voltage, so the run stops at the charge state corresponding to `cutoff + I·R`.
   Using the same R at both ends is what keeps lead resistance from biasing the
   answer: it shifts the current reading and the target together.
4. **Capacity is learned, not assumed.** After 10 % of charge travel,
   `Q = Ah drawn / charge travelled`. From then on the curve only has to get the
   *shape* right, not the absolute calibration — and **an estimate exists with no
   rated capacity entered at all**, which the old version could not do.
5. `remaining = (charge now − charge at cutoff) × Q / I`.

Fallbacks are explicit rather than silent: Custom chemistry has no curve, and a
run whose R was never measurable never invents one — both drop to the old
rated-capacity estimate, and the UI labels that figure "(from the rating)".

Byproducts worth having, all on the result and in the CSV: **pack + lead
resistance**, the **charge span the run covered** (a partial discharge is exactly
when the measured Ah understates the pack, so the state-of-health row should not
be read without it), and an **implied full capacity** extrapolated from that span.

Honest limits: these are curves for a chemistry *family*, not for your cell, so
every charge-state figure in the UI is prefixed "~". LiFePO4's plateau spans
~150 mV between 20 % and 90 %, so its charge readout is coarse mid-run and only
sharpens at the knee — the learned-capacity term is doing most of the work there.
R is measured once at switch-on and held, though it rises as a pack empties;
there is only ever one open-circuit reading to measure it against.

## 5. Open questions (answers change the plan)

1. Which chemistries do you actually test? (Trims/extends the preset list.)
2. Is a discharge-current *profile* (e.g. step/pulse discharge) ever needed, or
   is constant-current sufficient? (Engine hooks differ.)
3. Should the phone simulator app also get a battery model, so the BLE path can
   be end-to-end tested before a real EL15/pack is on the bench?
4. Rest/recovery measurement after cutoff: keep (adds ~60 s per test) or drop?
5. Max cell count 20S caps at 60 V anyway for Li-ion (14S) — is >48 V lead-acid
   (24 cells) in scope? If so the cells range needs to be per-chemistry.

---

*Written 2026-07-21. Companion docs: QA_GUIDE.md (test matrix — extend §5 with
the battery test when Phase 2 lands), QA_REPORT.md (M5 SD-stub finding closes
with Phase 0).*
