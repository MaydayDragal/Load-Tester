# EL15 Controller Firmware — QA Report (code-level audit)

Date: 2026-07-21 · Branch `claude/android-apk-load-tester-k82q4g` (HEAD `d65d430`)
Scope: full static audit of `firmware/src/*` + build verification, driven section-by-section
by `firmware/QA_GUIDE.md`. On-device items (touch feel, live BLE data, real EL15) are out of
scope here and remain on the guide's hardware checklist.

Build: **PASS** — `pio run` SUCCESS (10.6 s incremental), RAM 8.0 %, flash 1,402,971 B of
3 MB `huge_app` slot. Resolved libs GFX 1.6.7 / NimBLE 2.5.0 / LVGL 8.4.0 — exactly the
versions §3 says the code is adapted to. No warnings surfaced in the summary output.

---

## Verified correct (no action needed)

- **Wire protocol (§7)** — every command frame is byte-exact vs the guide table
  (`el15_protocol.h:23-29`): POLL/LOAD_ON/LOAD_OFF/LOCK, mode + setpoint prefixes,
  float32 LE setpoint. Mode IDs match (CC 01, CAP 02, CV 09, DCR 0A, CR 11, CP 19).
  Status parse offsets (V@7, I@11, runtime@15, b5/b6 flag decode, CAP/DCR tails),
  header `DF 07 03 08`, and sum-&-0xFF CRC all match the documented layout.
- **Reassembly + resync (§7)** — `el15_client.cpp:188-206` accumulates across
  notifications, parses only header-aligned 28-byte frames, drop-one-byte resyncs,
  handles back-to-back frames in one notification, and guards the 64-byte buffer.
  Works at both MTU 247 (requested, `el15_client.cpp:45`) and a 23-byte fallback.
- **Threading contract (§4)** — all NimBLE callbacks (scan result, disconnect, notify)
  only enqueue onto `evtQueue_`; everything downstream (LVGL, test engine) runs on the
  loop task via `drainEvents()`. Service discovery is on the loop task in `connectTo()`.
  No violation found anywhere.
- **Random-address connect (d65d430)** — scan stores `NimBLEAddress` with type; connect
  reuses it, falls back to public, logs `[ble] connecting to <addr> (addr type N)`.
- **Scan filter/dedup (§5.2)** — unnamed advertisers dropped at the source
  (`el15_client.cpp:69`), dedup by address across the scan (`el15_client.cpp:99-106`),
  Demo row pinned (child 0 preserved by `clearDevices`). Status strings match the guide
  ("Not an EL15 (no FFF0)", "EL15 characteristics missing", "Connect failed").
- **Sweep safety clamps (§6)** — `min(0.8×fuse, 12 A, 150 W ÷ Voc, 40 A)` exactly
  (`resistance_test.h:113-115`); 0.1–60 V prime window aborts; per-step re-clamp against
  live voltage; `finishSafely()` (LOAD_OFF + setpoint 0) runs on stop, complete, and
  every abort path; a protection warning in any status packet aborts the test.
  The engine commands **CC**, never an "RT" device mode.
- **Least-squares math (§1/§5.8)** — slope/intercept/R² and the reliability rule
  (n≥3, slope<0, R²≥0.90, spread>0.05 A) are correctly implemented; demo sweep will
  recover ≈0.35 Ω / ≈12.6 V / R²≈1.
- **Teardown ordering (§6)** — `stopAll()` captures `wasBusy` before `stop()`;
  BLE drop mid-test stops the engine on the loop task (`main.cpp:105`);
  `shutdownAndDisconnect()` pushes LOAD_OFF and waits 40 ms before dropping the link.
- **Setpoint echo suppression (§8)** — device echoes are ignored while on Adjust, while
  the keypad is open, and in RT mode (`ui.cpp:1177-1178`).
- **Load button reflects hardware state** — visuals keyed off `s.loadOn` from packets,
  not the tap (`ui.cpp:1140`, `refreshMonitor`).
- **Display/touch bring-up** — draw-buffer alloc checked before use, `LV_COLOR_16_SWAP 0`
  paired with `draw16bitRGBBitmap`, TCA9554 panel enable, FT3168 `0xA5=Active` init,
  hold-last-state on transient I²C failure. All consistent with §8's notes.

---

## Findings

Severity: **H** = functional bug, **M** = behavior/spec deviation worth fixing,
**L** = polish / doc mismatch. "Guide" = QA_GUIDE.md section.

### H1 — CR mode gets the wrong unit config everywhere (unit string mismatch)
`ui.cpp:110-113` `modeUnit()` returns `el15::setpointInfo(curMode).unit`, which for
MODE_CR is the **UTF-8 "Ω"** (`el15_protocol.h:79`) — but `unitCfg()` (`ui.cpp:103-108`)
matches the ASCII string `"ohm"`. `"Ω"` matches nothing and falls through to the **A**
default. Consequences in CR mode:
- Adjust clamps to **0–40** instead of 0.05–9999 Ω (guide §5.5) — can't set CR above 40 Ω.
- Step chips are 0.01/0.1/1 instead of 0.1/1/10; decimals 2 instead of 1.
- Keypad presets are 0.5/1/2/5 (the A presets) instead of 1/5/10/50.
- The Set bar, Adjust unit, caption, and keypad unit render the raw "Ω" glyph, which
  Montserrat lacks → **tofu box** (§8 explicitly says the UI must use "ohm").
The picker tile (`MU[]`, `ui.cpp:1069`) correctly uses "ohm" — only `modeUnit()` leaks
the protocol glyph. Fix: map MODE_CR → `"ohm"` inside `modeUnit()`.

### H2 — STOP (and disconnect) during a sweep leaves the R-Test UI stuck on "RUNNING"
`ResistanceTest::stop()` (`resistance_test.h:61-65`) fires **no callback**, and the STOP
button handler (`ui.cpp:742`) only calls `A.stopRTest()` — nothing resets `rtPhase`.
`rtPhase` is only cleared by `onTestError`, `onTestComplete`, or the "New test" button
(which lives on the **Result** box). After STOP the screen shows the RUNNING card +
spinner forever; same after a BLE drop mid-test (`main.cpp:105` stops the engine
silently). The load itself IS turned off (engine side is safe) — this is UI-only, but it
directly fails guide §5.8 "STOP … returns to Idle". Fix: in the STOP handler (and ideally
in a new engine `onStopped` path) set `rtPhase = RT_IDLE; refreshRtest();`.

### H3 — Computed layout overflow: Menu overlay row 3 and keypad bottom row are clipped
Both containers are non-scrollable (`cont()` clears `LV_OBJ_FLAG_SCROLLABLE`) and their
content exceeds the available height on the 368×448 panel:
- **Menu** (`ui.cpp:891-928`): grid height = 448 − 24 (pad) − 40 (header) − 10 (gap) =
  **374 px**; six 164×150 tiles wrap into 3 rows needing 3×150 + 2×8 = **466 px** →
  ~92 px overflow. The third row (**R-Test, Connect**) shows only its top ~58 px.
- **Keypad** (`ui.cpp:1009-1023`): key pad gets ≈ 424 − header 40 − display ~78 −
  presets 44 − SET 58 − gaps 32 = **≈172 px**; 4 rows of 62 px keys + gaps need
  **266 px** → the `. 0 ⌫` row is mostly/entirely clipped and untappable.
Notably the mode-picker grid *was* given `LV_OBJ_FLAG_SCROLLABLE` (`ui.cpp:1064`) even
though it fits — these two were not. Numbers are computed from code; **confirm with a
photo/screenshot on device**, then either shrink tiles/keys or make the grids scrollable.
This would fail §9 "Every screen renders within the 368×448 bounds" and §5.6 keypad tests.

### M1 — Fault banner does not latch (deviates from §6)
`ui.cpp:1185-1189` shows the banner while a packet carries a warning and **hides it as
soon as a clean packet arrives**; the tap handler (`ui.cpp:325`) merely hides it until
the next warning packet re-shows it (≤500 ms later while faulted). The load-ON gate
(`ui.cpp:442`) likewise tracks the **live** `lastStatus.warning`, not a latch. §6
specifies: banner latches, load blocked *while latched*, tap clears. Current behavior is
"mirror the device" — arguably defensible, but it is not what the guide promises; if a
trip is transient the banner can flash and vanish before the user ever sees why the load
dropped. Decide which semantics are intended and align code or guide.

### M2 — Synchronous BLE connect freezes the whole UI (no "Connecting…" ever paints)
`connectTo()` (`el15_client.cpp:130-159`) runs inside an LVGL click callback, i.e. inside
`lv_timer_handler()`. The "Connecting…" label is set before `client_->connect()` but the
next render only happens **after** connect returns — so during the attempt the screen is
frozen and touch is dead. No `setConnectTimeout()` is set anywhere; NimBLE 2.x defaults
to ~30 s, so a vanished peer freezes the UI for up to ~30 s. Guide §5.2 expects a visible
"Connecting…" state. Minimum fix: `client_->setConnectTimeout(5000)` + accept the short
freeze; better: make connect asynchronous.

### M3 — Scan end is never reported; status stays "Scanning…" forever
No `onScanEnd` override exists (`el15_client.cpp:15-19`); after the 8 s scan window the
state remains `SCANNING` and the Connect screen shows "Scanning…" (amber dot)
indefinitely until the user taps something. Add `onScanEnd` → enqueue → `setState(IDLE,
"Scan finished")`.

### M4 — No feedback for ~3.7 s after starting a sweep; errors invisible from Monitor
`rtPhase = RT_RUN` is only set by the **first `onTestProgress`** (`ui.cpp:1237-1239`),
which fires after prime (1300 ms) + settle (900 ms) + collect (1500 ms) ≈ **3.7 s**. Until
then "Start sweep"/"RUN TEST" appears to do nothing (guide §5.8 promises an immediate
Running screen). Worse, `onTestError` (`ui.cpp:1268-1272`) writes to a label on the
R-Test screen but doesn't navigate — a sweep started from the Monitor RT path that aborts
during priming (e.g. Voc out of range) fails **completely silently**. Fix: set
`rtPhase = RT_RUN` + `showScreen(SCR_RTEST)` in the start action, and navigate on error.

### M5 — "Save to SD" shows no confirmation at all (guide's stub description is wrong)
Guide §1/§8 says the stub "flips to `Saved — RTEST_NNN.csv`". No such string exists in
the codebase; the save handler (`ui.cpp:772-776`) sets `rtSaved` (making further taps
silent no-ops) and `rtSeq++` (write-only variable) but **never changes the button label**.
So the button appears to simply do nothing. Either add the confirmation label (and keep
it honest about being a stub) or fix the guide. `rtSeq` is currently dead state.

### M6 — Keypad SET bypasses range clamp and rounding
`kpSet()` (`ui.cpp:959-964`) applies `atof()` raw: typing 5000 in CC sends
`setSetpoint(5000)`. The ± stepper clamps and rounds (`stepApply`, `ui.cpp:450-461`);
the keypad doesn't, contradicting §5.5's "clamped to the unit range" and widening §8's
"ranges exceed hardware ratings" risk. Clamp/round in `kpSet()` with the same `UnitCfg`.

### M7 — Tapping a device row while already connected desyncs client state
Device rows stay listed after connecting (cleared only by a new scan). Tapping one while
connected: `connect()` on an already-connected client fails → `setState(IDLE, "Connect
failed")` — but the **old link stays up** with notifications subscribed, while polling
stops (`loopTick` gates on `CONNECTED`) and the UI claims disconnected. Guard `connectTo`
(disconnect first, or ignore when `CONNECTED`).

### M8 — Manual controls are not locked out during a running sweep
While `g_test.running()`, the Monitor load button, Adjust ±/keypad, and the mode picker
all still send commands (load toggle mid-sweep silently corrupts the fit; the engine only
re-issues `setLoad(true)` on step 0). Not a hardware hazard (device limits + clamps still
apply) but a data-integrity gap §6 doesn't cover. Suggest gating on `running()` like the
Android app locks its UI.

### Low / polish

- **L1** Fuse keypad path is dead code: `openKeypad(2)` is never called (only
  `openKeypad(1)` at `ui.cpp:522`), so the fuse can only be set by cycling; guide §5.6
  says the keypad applies "setpoint or fuse".
- **L2** Demo simulator state persists across sessions: `startDemo()` doesn't reset the
  sim, so a load left ON (with runtime/Ah accumulating state) reappears ON at the next
  "Demo Simulator" connect (`main.cpp:55-62`; `El15Simulator` has no reset()).
- **L3** Guide §5.8 says "tap the status bar to edit" the demo circuit — **no such
  editor exists** in the firmware; emf/seriesR are compile-time constants. The >60 V
  abort and the 150 W sweep clamp are therefore untestable in demo without a code edit.
- **L4** DCR hero shows `dcrI1` (`ui.cpp:1159`) while guide §5.3 says DCR shows "zero
  current" (the protocol layer does zero `s.current`). Decide which is intended.
- **L5** Unset fuse renders faint gray "--" (`ui.cpp:796`); guide §5.8 says "red until set".
- **L6** `onTestProgress` force-navigates to the R-Test screen on **every** callback
  (`ui.cpp:1239`) — a user checking Monitor/Graph mid-test is yanked back every ~2.4 s.
- **L7** `rtStatusLbl` error text is never hidden once shown (`ui.cpp:1268-1272`) — the
  last abort message stays under the Idle/Result boxes forever.
- **L8** Informative connect-failure states ("Not an EL15 (no FFF0)") are overwritten a
  moment later by the async disconnect event's "Disconnected" (`el15_client.cpp:150-166`).
- **L9** On disconnect the history counter resets but painted chart points remain; the
  Graph shows the stale trace until new data arrives (`ui.cpp:594` early-return).
- **L10** `startDemo()` doesn't stop an in-progress scan; rows keep appending while in
  demo until the 8 s window lapses.
- **L11** Fan decode is 3 bits (0–7, `el15_protocol.h:197`) but the info bar prints
  "n/5" — a real device reporting 6/7 would render "7/5".
- **L12** "SINKING" chip is static; guide §5.3 says "pulsing" (cosmetic, guide overstates).

---

## Guide checklist (§9) — status after this audit

| Item | Code-level verdict |
|---|---|
| Build clean | ✅ PASS (this audit) |
| Screens within 368×448 | ❌ suspect — H3 (Menu row 3, keypad bottom row) |
| Scan named-only, dedup, Demo pinned | ✅ code-verified |
| Connect → auto-home; serial addr-type log | ✅ code-verified (but M2/M3 UX) |
| Live V/I/mode from peer | ⏳ hardware test still required (guide's key gap) |
| 7 modes select; unit/step follow | ❌ CR broken — H1 |
| Adjust exact/clamped/rounded | ⚠️ stepper yes; keypad no — M6 |
| Load tracks hardware; blocked when faulted | ✅ tracks echo; ⚠️ gate not latched — M1 |
| R-Test both entry paths; demo ≈0.35 Ω | ✅ engine verified; ⚠️ M4 feedback gap |
| STOP/finish leave load OFF | ✅ engine-side; ❌ UI stuck after STOP — H2 |
| Save-to-SD stub | ⚠️ stub confirmed, but no confirmation UI — M5 |
| Graph auto-scale/scroll | ✅ code-verified (L9 stale-trace nit) |
| Fault banner latches/gates/clears | ❌ not latched — M1 |
| Disconnect mid-test stops load | ✅ user-initiated paths; spontaneous drop relies on device timeout (unfixable from central) |
| Sweep clamps / out-of-range abort | ✅ code-verified exactly per §6 |

Top recommended fixes, in order: **H1, H2, M4** (small, high-impact), then **H3** after
an on-device screenshot confirms the overflow, then M1 semantics decision, M2/M3 BLE UX.
