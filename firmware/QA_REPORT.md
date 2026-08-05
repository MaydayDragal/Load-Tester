# EL15 Controller Firmware — QA Report (code-level audit)

> **This is a dated snapshot, not a live defect list.** The audit below was run
> on 2026-07-21 against `d65d430`; most of it has since been fixed. Jump to
> **[§ Resolution status](#resolution-status-as-of-2026-08-01)** at the end for
> what is still open as of `6adea41`. Sections above that point are preserved
> verbatim as the historical record — several of them describe code (the demo
> simulator, the SD stub, the R²-based reliability rule) that no longer exists.
> A second, stability-focused audit was run on 2026-08-05 — see
> **[§ Stability audit (2026-08-05)](#stability-audit-2026-08-05)** at the end.

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

---

## Resolution status (as of 2026-08-01)

Re-checked against `6adea41` by reading the current `firmware/src/*`. "Fixed" =
the specific code path named in the finding now behaves as the finding asked.
Nothing here was re-tested on hardware — this is a static re-read, same as the
original audit.

Build re-verified at this commit: **PASS**, clean under `-Wall -Wextra`,
flash 2,113,255 B of the 3 MB `huge_app` slot, RAM 17.8 % static.

| # | Finding | Status |
|---|---|---|
| **H1** | CR mode gets the wrong unit config (Ω vs "ohm") | ✅ **Fixed** — `modeUnit()` maps MODE_CR to `"ohm"`, with a comment recording why |
| **H2** | STOP leaves the R-Test UI stuck on "RUNNING" | ✅ **Fixed** — the STOP handler sets `rtPhase = RT_IDLE`; e-stop, link-guard, error and disconnect paths do the same |
| **H3** | Menu row 3 / keypad bottom row clipped | ✅ **Fixed** — Menu is now 8 tiles at 164×84 in a **scrollable** grid; sized in a comment to fit 4 rows |
| **M1** | Fault banner does not latch | ⚖️ **Decided, not changed** — the banner deliberately mirrors the device. The load-ON gate tracks the live warning; load-**OFF** is never gated. QA_GUIDE §6 now documents this as the intended semantics |
| **M2** | Synchronous connect freezes the UI for ~30 s | ✅ **Fixed** — `connectTimeoutMs_` defaults to 4000 with a bounded retry count; the guard drops it to a single 4 s attempt during recovery |
| **M3** | Scan end never reported; status stays "Scanning…" | ❌ **Still open** — there is no `onScanEnd` override; an expired scan window leaves `state_ == SCANNING` |
| **M4** | No feedback for ~3.7 s after starting a sweep | ✅ **Fixed** — `enterRtRun()` sets `RT_RUN` and shows `SCR_RTEST` at start; errors navigate and surface in `rtStatusLbl` |
| **M5** | "Save to SD" shows no confirmation | ✅ **Fixed** — SD is real (`sd_card.cpp` + `report.h`); the button arms, then goes green with the file name or red with the reason |
| **M6** | Keypad SET bypasses clamp/rounding | ✅ **Fixed** — every keypad target clamps (wire length, battery cutoff/current), and edits are refused outright while an engine runs |
| **M7** | Tapping a device row while connected desyncs state | ✅ **Fixed in effect** — the client is kept and reused on failure rather than deleted, and the connect action stands the guard down first; the old "silent live link + UI says disconnected" split no longer occurs |
| **M8** | Manual controls not locked out during a sweep | ✅ **Fixed** — `engineBusy()` gates the load bar, Adjust, the keypad and the mode picker |
| **L1** | Fuse keypad path is dead code | ❌ **Still open** — `openKeypad(2)` is never called; the fuse is cycle-only |
| **L2** | Demo simulator state persists across sessions | ➖ **Moot** — the on-device simulator was removed |
| **L3** | No demo-circuit editor | ➖ **Moot** — same removal; bench-testing uses the phone simulator |
| **L4** | DCR hero shows `dcrI1`, guide says zero current | ❌ **Still open** — `shownI` is `dcrI1` in DCR mode. Decide which is intended |
| **L5** | Unset fuse renders faint gray "--", guide said red | ✅ **Guide aligned** — the code is unchanged; QA_GUIDE no longer claims red |
| **L6** | `onTestProgress` force-navigates on every callback | ✅ **Fixed 2026-08-01** — the force-navigate is gone and a running-test chip in the status strip is the way back, exactly as FEATURE_IDEAS §10 proposed |
| **L7** | R-Test error text never hidden once shown | 🟡 **Partly fixed** — `enterRtRun()` clears the stale label at the start of a run; it still persists after an error until the next run |
| **L8** | Informative connect-failure states overwritten | ❌ **Likely still open** — the "no FFF0" / "characteristics missing" paths call `disconnect()` before `setState()`, so the async disconnect event's "Disconnected" can land afterwards |
| **L9** | Stale chart trace after disconnect | ❓ **Not re-verified** |
| **L10** | `startDemo()` doesn't stop a scan | ➖ **Moot** — demo removed |
| **L11** | Fan decoded 0–7 but printed "n/5" | ✅ **Fixed** — clamped to `FAN_SPEED_MAX` and shown as a percentage |
| **L12** | "SINKING" chip static, guide said pulsing | ✅ **Guide aligned** — still static by design; FEATURE_IDEAS §11 tracks the animation as a nice-to-have |

### New findings from this re-read

| # | Finding | Severity |
|---|---|---|
| **N1** | **The Android app (`app/`) never got the command-checksum fix.** `El15Protocol.kt` still builds `LOAD_ON/LOAD_OFF/LOCK/modeCommand/setpointCommand` with no trailing sum-to-zero byte — exactly the defect that made load control silently do nothing on the real EL15 until it was fixed in the firmware on 2026-07-24. Only POLL (whose checksum was captured whole) works. | **H** — app-side |
| **N2** | `display.cpp` constructs the panel bus with `is_shared_interface = true`, whose only reason was sharing the SPI host with the SD card. The card moved to bit-banged software SPI on 2026-07-24, so this now costs one bus-lock round-trip per flush chunk for nothing. Comment updated; behavior deliberately left alone pending a panel re-verify on hardware. | **L** — perf |
| **N3** | `sd::unmount()` was unused outside `EL15_SDTEST`, producing the build's only warning. Marked `[[maybe_unused]]` so the tree is warning-free again. | **L** — fixed |
| **N4** | **Any non-CONNECTED BLE state was treated as a link drop** (`main.cpp` `onState`), so the user's own SCANNING/CONNECTING transitions tore down a running test — opening Connect and tapping Scan mid-discharge ended it, and mid-priming it died as "Cancelled" leaving nothing to save. Now latches `g_wasConnected` and acts only on a real drop from a live link. | **H** — fixed 2026-08-01 |
| **N5** | **SdFat was never given a clock**, so every file it created carried the card's default date and a PC's file listing showed the wrong date even though the CSV body and Settings clock were correct. `FsDateTime::setCallback` now reads the PCF85063. | **M** — fixed 2026-08-01 |
| **N6** | **The card was mounted once and never re-probed**, so an eject left SdFat serving a cached FAT and a re-inserted card was ignored until reboot. Entry points now probe with CMD10 and re-init on failure. | **M** — fixed 2026-08-01 |

---

## Stability audit (2026-08-05)

Date: 2026-08-05 · HEAD `1d4a21e` · Build: **PASS** (`pio run` 11.4 s, RAM 19.8 %
static / 65,028 B, flash 69.7 % of the 3 MB `huge_app` slot, no warnings).

Method: six independent review passes over `firmware/src/*` (heap/memory,
concurrency+task contexts, blocking/watchdog, safety state machines,
arithmetic/time, peripheral robustness), findings then adversarially re-traced.
**CONFIRMED** = the full code path was independently re-traced end-to-end (or
directly re-read during report assembly) and the scenario holds. **PLAUSIBLE** =
found and internally consistent, but not independently re-verified — treat as a
credible lead, not a proven defect. 27 raw findings deduplicated to 19.

The dominant theme: **mode commands got the closed-loop confirm-and-retry
treatment (HANDOVER §18) but LOAD ON/OFF transitions did not** — every
`setLoad()` in the engines, the power-off path, and the test-exit path is still
a single fire-and-forget write-no-response, on hardware measured to silently
drop ~2 of 9 such writes. S3/S4/S12/S13 are all instances. The second theme:
the link guard's protection can be dismantled by ordinary code paths (S2/S6)
while the load is unconfirmed.

### Findings table

| # | Sev | Status | Where | Finding |
|---|---|---|---|---|
| **S1** | **H** | CONFIRMED | `ui.cpp:3744-3755` | NTP-sync DONE path runs `delay(1600); ESP.restart()` unconditionally. The sync *button* is gated on `engineBusy() \|\| lastLoadOn`, but nothing stops a test start / manual LOAD ON *during* the 5–27 s sync window (`startBatt`, `ui.cpp:2780`; manual toggle, `ui.cpp:879`; none check `net::busy()`). Sync lands → reboot mid-discharge, no LOAD_OFF pushed, recovery reduced to the tap-to-act boot banner. |
| **S2** | **H** | CONFIRMED | `main.cpp:500` | `onBattError` disarms the link guard (and wipes the NVS in-flight flag, `link_guard.h:51-56`) whenever both engines are idle, *before* `onConnState` runs at `main.cpp:441`. On a BLE drop during PRIMING, `pause()` fails → `stop("Connection lost")` → `onError` → disarm; the guard's reconnect-and-kill never starts and the boot recovery flag is gone. The justifying comment ("handleStatus re-arms on the next packet") is false exactly when the link is dead. |
| **S3** | **H** | CONFIRMED | `capacity_test.h:591-595` | Every test exit ends in `finishSafely()`: one unconfirmed `setLoad(false)` + `setSetpoint(0)` pair. No caller confirms against `s.loadOn` or retries — unlike the guard's `forceOff()` (3×, `link_guard.h:174-180`) and unlike mode commands. `powerOffSafely()` (`main.cpp:231-248`) is the worst case: it clears the NVS recovery flag on the assumption the write landed, then cuts its own power. Same pattern in `resistance_test.h:477-481`, `emergencyStop()`, `stopAll()`. |
| **S4** | M | CONFIRMED | `capacity_test.h:533, 270` | The capacity engine never verifies the load turned ON: `finishPriming()` and `resume()` fire one `setLoad(true)` and never read `s.loadOn` (the R-test re-asserts at 400 ms, `resistance_test.h:191-199`; hardware-measured drops make this a several-percent event). Dropped LOAD_ON → phantom "discharge" at 0 A that only the 2.5×-duration cap ends, up to 48–72 h later, with a garbage 0 Ah result. |
| **S5** | M | CONFIRMED | `capacity_test.h:327-345` | No telemetry-staleness watchdog while DISCHARGING: cutoff, protection-trip and Ah-cap all live in `onStatus()`; `tick()` checks only the duration cap; the client reconciles only `isConnected()` (`el15_client.cpp:523`) and the guard fires only on a disconnect *event*. A mute-but-connected peer (wedge, lost CCCD, persistent checksum-failing corruption) leaves the load sinking, blind to cutoff, until the multi-hour cap. PRIMING has exactly this deadline (`PRIME_MAX_MS`); DISCHARGING does not. |
| **S6** | M | CONFIRMED | `link_guard.h:66-73` | `standDown()` — invoked by the UI's Scan/Connect actions (`main.cpp:307-308`), reachable mid-recovery by design — unconditionally runs `prefs::clearInFlight()`, erasing the crash-recovery flag while the load may still be ON. After a failed rescue scan, a brownout/power-off boots with `inFlight==NONE` and no recovery offer. The comment two lines above ("the crash/reboot flag still covers a power loss") is contradicted by line 72. |
| **S7** | M | CONFIRMED | `ui.cpp:2931, 1464, 2246` | SD saves are reachable while the *other* engine drives the load: save buttons gate only on `battSaved`/`rtSaved`, `engineBusy()` excludes RESULT phases, and Settings' "Check card" has no gate at all. A BATT save blocks the loop task synchronously for ~20–80 s (soft-SPI write + CRC verify ×2 attempts) — BOOT e-stop, guard tick, BLE drain, both engines and LVGL all frozen with current flowing; TWDT stays fed so it is a silent supervision outage. |
| **S8** | M | CONFIRMED | `ui.cpp:3869, 1719` | Scan results are unbounded on a heap with ~35 KB headroom: ~1–2 KB of LVGL objects per named advertiser plus an unchecked `new std::string`, rows freed only on the *next* scan, and NimBLE's own scan cache grows too (`setMaxResults(0)` never called). A BLE-dense environment fragments or exhausts the heap exactly before the connect attempt (HCI 0x3e), including the post-link-loss rescue scan; dense enough → `lv_obj_create` NULL / abort panic. |
| **S9** | L | CONFIRMED (mechanism) | `capacity_test.h:305, 327` | No `isfinite()` anywhere on the telemetry path (raw float32 memcpy in `el15_protocol.h`). One NaN current sample makes `ah_`/`wh_` NaN **permanently** — the Ah safety cap (`ah_ >= capAh_`) is dead for the rest of the run; a NaN voltage defeats both cutoff comparisons while present and resets the debounce. Trigger needs the peer to emit NaN through a valid checksum (unconfirmed on real hardware) — but the fix is a one-line gate in `onStatus()`. |
| **S10** | M | PLAUSIBLE | `ui.cpp:2172, 3204` | The Wi-Fi gates test `engineBusy() \|\| lastLoadOn`, but `onConnState` force-clears `lastLoadOn` on any disconnect (`ui.cpp:3854`) — so sync/scan pass exactly when the load state is *unknown and possibly ON* (guard RECOVERING/FAILED). esp_wifi then holds ~50 KB heap + the shared C6 antenna, so every guard reconnect attempt fails while current flows; a successful sync reboots on top (S1). |
| **S11** | M | PLAUSIBLE | `display.cpp:604-630, 465-468` | `setLowMemMode()` frees the active draw buffer *before* allocating the replacement; if every ladder rung fails it returns with the dangling buffer still registered — LVGL keeps rendering into freed heap (the "next flush would deref null" comment is wrong: it's dangling, not null). Boot variant: if the initial 47 KB alloc fails, `display::begin()` returns without registering a display and `ui::begin()` derefs `lv_scr_act()==NULL` → deterministic bootloop (latent until the RAM budget shrinks). |
| **S12** | L | PLAUSIBLE | `link_guard.h:126-134` | On reconnect the guard fires `forceOff()` (3 blind pairs), then immediately disarms, clears NVS and announces "Reconnected and shut the load down" — without ever confirming `s.loadOn==false` from telemetry on a link that "has only just come back". `handleStatus` re-arms if loadOn persists, but nothing re-commands OFF and the green resolution banner stands. |
| **S13** | L | PLAUSIBLE | `main.cpp:161-204` | Controller-battery pause (≤8 %) and auto-resume use the same threshold with no hysteresis: gauge dither at the boundary toggles LOAD_OFF/LOAD_ON every few seconds for hours (relay/FET chatter, alarm spam) — and every resume is an unconfirmed LOAD_ON (S4). |
| **S14** | L | PLAUSIBLE | `display.cpp:251` + `main.cpp:189` | A NACKed fuel-gauge percent read maps to 0 % while the function still returns true → after the 3-sample debounce, a healthy discharge is force-paused (and stays paused while the reads stay bad). The mV path has a validity guard; the percent path doesn't. |
| **S15** | L | PLAUSIBLE | `sd_card.cpp:367-383` | `nextIndex()` walks *every* root-directory entry over ~250 kHz soft SPI with no `delay(1)` — the module's other long loops feed the idle-task TWDT explicitly. A card with thousands of root files can starve the idle task past 5 s → reset mid-save. |
| **S16** | L | PLAUSIBLE | `audio.cpp:40` | TCA9554 read-modify-write falls back to `v=0x00` when the read-back returns no data, then writes it — dropping the AMOLED panel-enable/reset bits set moments earlier. One glitched I2C read at boot → black screen with the firmware fully alive (indistinguishable from a hang to the operator). |
| **S17** | L | PLAUSIBLE | `capacity_test.h:328` | The single-sample instant cutoff (`voltage < cutoffV-0.3`) trusts one frame guarded only by the 8-bit additive checksum (~1/256 corrupt frames pass). Over ~1.7 M frames of a 23 h run, one passing corruption ends the test with a plausible-looking wrong result. Fail-safe direction, but a multi-day run is silently lost. |
| **S18** | L | PLAUSIBLE | `netclock.cpp:148` | `startScan()` anchors `g_scanNextTryMs=0`, so `(int32_t)(millis()-0) < 0` wedges the scan for the entire uptime window ~day 24.9–49.7: `net::busy()` stuck true, no more clock syncs, Wi-Fi radio left up beside BLE. |
| **S19** | L | PLAUSIBLE | `el15_client.cpp:586-587` | `pollHoldUntilMs_` is re-anchored only at connect and on control writes; ~24.85 days connected with no command and the signed-wrap gate goes negative — polling (hence all telemetry) silently stops while the UI shows Connected. Mirror of the initial-anchor bug already fixed at `el15_client.cpp:306-311`. |

### Suggested fix order

1. **Close the LOAD-transition loop** (S3, S4, S12, S13-resume): confirm every
   `setLoad()` against the next status packet's `s.loadOn` and re-assert on
   mismatch — the exact pattern the mode commands already got in HANDOVER §18,
   and the R-test's 400 ms re-assert already proves out. This single change
   retires the worst halves of four findings.
2. **Stop dismantling the guard while the load is unconfirmed** (S2, S6):
   don't `disarm()`/`clearInFlight()` unless telemetry has confirmed loadOn
   false or the guard itself completed; in `onBattError`, skip the disarm when
   the link is down (or run it after `onConnState`).
3. **Gate the restart and the blockers on load state** (S1, S7, S10): make the
   NTP DONE path refuse to reboot while `engineBusy() || loadHot()` (finish the
   sync, skip the restart); gate SD save/check on the same predicate; gate test
   starts on `net::busy()`.
4. **One-line hardening**: `isfinite()` on V/I in `onStatus()` (S9); cap scan
   rows + `setMaxResults(0)` (S8); `delay(1)` in `nextIndex()` (S15); resume
   hysteresis at e.g. 12 % (S13); anchor fixes for the two 24.9-day wraps
   (S18, S19).
5. **Staleness watchdog while DISCHARGING** (S5): pause + guard-style recovery
   after N seconds without a valid frame — PRIMING already has the template.

On-device items (S4's drop rate, S5's mute-link plausibility, S16's I2C glitch)
stay on the QA_GUIDE hardware checklist; nothing here is verified on hardware.
