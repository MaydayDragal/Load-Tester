# Battery Capacity Test — Overhaul Gameplan

Goal: turn CAP from a bare device mode into a full battery bench test — any pack
size/voltage the EL15 can handle (0.1–60 V, 12 A, 150 W), automatic minimum-
voltage cutoff, continuous V/I/Ah/Wh/temp logging, live discharge curve, and
results savable to SD like the R-test report (which itself becomes real, not a
stub, as part of this work).

---

## 1. What exists today (starting point)

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
  and an ETA/progress estimate while running.
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

### 2.4 SD card (Phase 0 — also unblocks the R-test stub)
- ESP32-C6 has **no SDMMC host** — the slot must run in SPI mode. Pins from the
  TODO map to SPI as SCK=11, MOSI(CMD)=10, MISO(D0)=18; **CS pin must be
  verified** against Waveshare's pin_config.h (open question — possibly D3 on a
  GPIO or the TCA9554 expander).
- `sd::begin()` lazy-mounts FAT; writes only from the loop task; card-absent
  and write-failure states surface honestly in the UI (no fake "Saved").
- Naming: `RTEST_NNN.csv` / `BATT_NNN.csv`, next index by directory scan.
  Metadata header rows (config, firmware version, RTC timestamp if set — CSV
  uses uptime seconds when the RTC is unset).
- Retro-fit the R-test save to write a real CSV (samples + summary + circuit
  estimate) — closes QA-report finding M5.

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

### 2.6 Persistence & clock (Phase 4)
- NVS: last battery config, sample rate, brightness.
- Settings gains "set clock" (simple hour/min/date steppers writing PCF85063)
  so logs get real timestamps; until then filenames use the NNN sequence.

## 3. Safety & failure-mode review (design-time)

| Risk | Mitigation |
|---|---|
| BLE drops mid-discharge — no supervisor for cutoff | Engine treats disconnect as abort; on reconnect-capable link, retry LOAD_OFF loop + full-width alert. Residual: device keeps sinking until its own UVP — document loudly; recommend setting the EL15's hardware UVP as backstop when testing real packs. |
| ESP reboot mid-test | NVS "test-in-progress" flag; on boot, alert + offer reconnect-and-stop. (Stretch goal.) |
| Wrong cell count / cutoff too low | Auto-suggest + plausibility warning at setup; hard floor 0.1 V. |
| Noise triggers early cutoff | 3-sample debounce; single-sample only below cutoff − 0.3 V. |
| SD removed / full mid-test | Stream failures flip the UI save state to "SD error — RAM curve retained"; result save can retry. |
| Long-test heap creep | Fixed buffers only; no allocation after start (R-test lesson). |

## 4. Phases & order of work

| Phase | Scope | Est. size |
|---|---|---|
| 0 | SD SPI bring-up, `sd::` module, real R-test CSV save, CS-pin verification | ~1 session |
| 1 | `capacity_test.h` engine + El15Simulator battery model (emf sags with drawn Ah along a simple SoC curve) so everything is testable on-device without hardware | ~1 session |
| 2 | SCR_BATT setup/running/result UI + charts + picker/Menu wiring | 1–2 sessions |
| 3 | CSV streaming during test + result save + file naming | ~0.5 session |
| 4 | NVS persistence, RTC set UI, QA-guide update, full QA pass | ~1 session |

Dependencies: 0 → 3; 1 → 2 → 3; 4 last. Phases 0 and 1 are independent and
could land in either order.

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
