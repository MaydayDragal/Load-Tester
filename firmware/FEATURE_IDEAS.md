# EL15 Controller — Feature Backlog

Candidate features for the ESP32-C6 controller. Effort: **S** = hours, **M** = a
session, **L** = multi-session. "Free" = needs no hardware you don't already have.

---

## 0. Hardware you already own but don't use yet

This is the headline: most of the board is idle. Every feature below marked
*(free)* rides on silicon already soldered down.

| Hardware | Status | What it unlocks |
|---|---|---|
| **WiFi 6 radio** | **in use** — NTP clock sync only (Settings ▸ Clock), radio powered per-op | web UI, REST/MQTT, OTA updates, cloud logging, alerts |
| **802.15.4 radio** (Thread/Zigbee/Matter) | completely unused | smart-home integration without WiFi |
| **QMI8658 6-axis IMU** | completely unused | auto-rotate, tap/shake gestures, wake-on-lift, e-stop shake |
| **SD card slot** | **in use** — CSV test reports on its own bit-banged SPI pins, verified on hardware | live streaming logs, history browser, load-profile replay |
| **PCF85063 RTC** | **in use** — read + settable via Wi-Fi NTP | real timestamps on logs (done); a manual set-time UI for benches with no Wi-Fi is still open |
| **AXP2101 PMIC** | **in use** — battery %/V/USB read, brownout auto-safe, load-safe power-off | runtime estimate, charge-current control, rail power-saving |
| **PWR + BOOT buttons** | **in use** — BOOT = hardware e-stop, PWR = sleep/wake + load-safe power-off | start/stop a test, screenshot, safe-mode-at-boot (§15) |
| **ES8311 audio codec + speaker** | **in use** — tap/button/complete/error/alarm tones, volume + mute | audible probe mode, cutoff warnings, spoken results (§14) |
| **USB-Serial/JTAG** | flashing + logs only | SCPI-style scripting API for lab automation |
| **RTC backup-battery pads** | unpopulated | keeps the clock running through a main-battery swap |

*(Table refreshed 2026-08-01 — audio, buttons, SD, RTC, PMIC and Wi-Fi have all
gone from "unused" to shipped since it was first written.)*

Board facts worth remembering (from Waveshare's spec): **512 KB on-chip HP SRAM
and NO PSRAM** (hence LVGL's partial draw buffer — a full 322 KB framebuffer
will never fit), 16 MB flash, and a Type-C port. Waveshare name the display
driver **CO5300** and the touch controller **CST820**; our code drives them via
Arduino_SH8601 and a FocalTech-compatible touch register map, which works on
this unit — see the notes in `board_config.h`.

---

## 1. New test modes

The sweep and discharge engines already in the tree do most of the heavy
lifting; several of these are small deltas on existing code.

| Feature | Effort | Notes |
|---|---|---|
| **Solar panel I-V + max-power-point curve** | **S** | The R-test *already* sweeps current and records V/I. Plot P = V·I, mark the peak → Vmp/Imp/Pmax/fill factor. Highest value-per-line-of-code in this list. |
| **PSU / charger characterisation** | M | Load-regulation curve, current-limit discovery, OCP/OVP trip point, drop-out behaviour. Same sweep engine, different reporting. |
| **Battery internal resistance (pulse IR)** | S–M | Distinct from the circuit R-test: short high-current pulse, measure ΔV/ΔI. The industry-standard cell-health number. |
| **Peukert exponent / capacity vs C-rate** | M | Run capacity tests at 2–3 currents, fit the exponent. Tells you real runtime at any load. |
| **State-of-health tracking** | S | ~~Compare measured Ah to rated Ah~~ **done** (result row + CSV, from the optional rated-capacity input). What is left is *trending* it across saved tests per battery ID, which needs on-device history. |
| **Parasitic-draw / standby test** | S | Very low current, very long duration, high-resolution Ah. Mostly a UI preset over the capacity engine. |
| **Step / pulse / profile discharge** | M | Programmable sequence (e.g. 1 A 30 s → 5 A 10 s, repeat). Simulates real loads. Engine hook exists in the capacity plan. |
| **Runtime-to-cutoff prediction** | S | Extrapolate from a partial discharge instead of running it to empty. |
| **Supercapacitor capacity** | S | Capacity engine with a different integration window. |
| **LED / diode I-V characterisation** | S | Sweep in low-current CC, plot forward curve. |
| **Wire ampacity / thermal derating** | M | Ramp current, watch temperature rise, stop at a limit. |
| **Auto protection-threshold discovery** | M | Slowly ramp until the EL15 trips, report the actual UVP/OCP point. |
| **Alternator / generator load bank test** | M | Sustained load with voltage-stability statistics. |

## 2. Data, logging & history

| Feature | Effort | Notes |
|---|---|---|
| **SD CSV logging** | M | Already planned (Phase 0/3). Unblocks everything else here. |
| **On-device test history browser** | M | Scroll past results, reopen a curve. Needs SD or NVS. |
| **Overlay/compare two tests** | M | Same-axes comparison — before/after a repair, battery A vs B. |
| **Named tests / battery IDs** | S | On-screen keyboard exists in LVGL; makes history meaningful. |
| **Pass/fail limits (go/no-go)** | S–M | "Fail if Ah < 80% of rated" — turns it into a production tool with a big green/red verdict. |
| **QR code of results on screen** | S | LVGL has a built-in QR widget; phone-scannable results with no cabling. |
| **Screenshot to SD** | S | Documentation/reporting. |
| **Statistics per test** | S | min/max/avg/stdev, ripple estimate. |
| **Anomaly detection** | M | Flag a sudden voltage step mid-discharge = likely bad cell/connection. |

## 3. Connectivity *(the WiFi radio is completely unused)*

| Feature | Effort | Notes |
|---|---|---|
| ~~NTP time sync~~ | **done** | Settings ▸ Clock: **scan and pick** a network (hidden/manual entry too), type the password, set UTC offset, "Sync clock now". Radio is powered only for the scan/sync and both are refused while a test runs (shared antenna with BLE). `netclock.cpp`. |
| **OTA firmware update** | S–M | No more USB cable for updates. |
| **Built-in web UI** | L | Full control + live charts from any browser; the big-screen companion this 1.8″ display can't be. |
| **REST API / MQTT publish** | M | Home Assistant, Grafana, InfluxDB, lab scripting. |
| **Push alert on completion/fault** | S–M | Long capacity tests run for hours — get told when it finishes or trips. |
| **SCPI-style USB command API** | M | Scriptable from Python over the existing USB CDC. The standard way bench gear automates. |
| **BLE peripheral mode** | M | Device re-broadcasts live data to a phone app while still driving the load. |
| **Matter/Thread** | L | Only if you want it in a smart-home ecosystem. |
| **Multi-load ganging** | L | Drive several EL15s for higher combined power. |

## 4. Safety & robustness

| Feature | Effort | Notes |
|---|---|---|
| ~~Link-loss auto-stop supervisor~~ | **done** | `link_guard.h`: whenever the load reports ON, a dropped link starts a reconnect-and-force-LOAD-OFF loop (8 tries) with a locked red banner + repeating alarm; gives up loudly rather than silently. |
| ~~Crash/reboot recovery~~ | **done** | NVS in-flight flag (written synchronously while energised) → on boot, offers "reconnect and force LOAD OFF" using the stored last device. `prefs` + `link_guard`. |
| ~~Controller-brownout auto-safe (AXP2101)~~ | **done** | `main.cpp monitorPower()`: the controller's OWN battery is watched (fuel-gauge % + VBAT floor, suppressed on USB); at the critical threshold it force-stops the load before the controller can brown out and strand it — the sibling of the link guard for "the controller's battery died". |
| ~~Load-safe power-off (AXP2101)~~ | **done** | Long-press PWR forces LOAD OFF and flushes it over BLE before `display::powerOff()` cuts the rails — the PMIC's own OFFLEVEL cutoff would skip that and strand an energised load. |
| **Temperature auto-abort** | S | User-set limit; stop before the load cooks. |
| **User soft limits** | S | Cap max current/power below hardware ratings for a given setup. |
| **Confirm dialog above a power threshold** | S | Guard rail for high-energy runs. |
| **Emergency stop gesture** | S | Shake (IMU) or long-press anywhere → LOAD OFF. |
| ~~Audible alert~~ | **done** | Trip/completion/e-stop tones via the on-board ES8311 — see §14. |
| **Screen lock during a test** | S | Stops accidental taps changing a running test. |

## 5. UI / UX

| Feature | Effort | Notes |
|---|---|---|
| ~~Persistent settings (NVS)~~ | **done** | `prefs.cpp`: brightness, volume/mute, sample rate, screen-protection, R-test + battery setup, Wi-Fi, last device all survive a reboot. Named/recallable *profiles* still open. |
| ~~Auto-dim / sleep + wake-on-tap~~ | **done** | Settings ▸ Screen protection: dim after 1/2/5 min idle, blank at 5x that, wake on any touch/button. Blank is suppressed while a test runs. `display.cpp`. |
| **Idle "watch face"** | S | Clock, battery, last result when nothing is connected. |
| **Screen auto-rotate (IMU)** | S | Free with the IMU. |
| **Customisable Menu tiles / favourites** | S | Put your two real workflows on the home screen. |
| **Light/high-contrast theme** | M | Bench lighting is brutal on a dark theme. |
| **Undo last setpoint change** | S | Cheap safety net. |
| **On-screen keyboard for names** | S | Prerequisite for named tests. |

## 6. Calibration & accuracy

| Feature | Effort | Notes |
|---|---|---|
| **2-point user calibration of V and I** | M | Correct against a reference DMM, store slope/offset in NVS. Turns "indication" into "measurement". |
| ~~Zero/tare offset~~ | **done** | R-test setup (2-wire): "Measure (short the probes)" runs a tare sweep, stores it in NVS, subtracts it from every later result and shows the raw figure alongside. |
| ~~4-wire (Kelvin) workflow support~~ | **done** | R-test setup has a 2-wire/4-wire toggle with hook-up guidance; the result carries the wiring and (2-wire) the tare. `resistance_test.h` + `ui.cpp`. |
| **Calibration due-date reminder** | S | If this ever does real QA work. |

## 7. Optional external hardware

| Feature | Effort | Notes |
|---|---|---|
| **External temp probe (DS18B20/thermocouple)** | M | Battery/pack temperature, not just the load's internal sensor. Enables true thermal cutoff. |
| **Relay board for charge/discharge cycling** | L | Automated cycle life testing — the flagship capability if you ever want it. |
| **Current clamp on the charge side** | M | Full round-trip efficiency measurement. |
| **Haptic motor** | S | Vibration feedback for taps/alerts (audio is already on-board — see §14). |

---

# Part II — UI & interaction redesign

The current UI is **entirely tap-and-navigate**: every destination is a Menu
tile or a button. On a 368×448 watch-sized panel that costs a lot of taps and
a lot of screen real estate. The ideas below are about density, gestures, and
making the *rounded* form factor work for us instead of against us.

## 8. Interaction model — gestures

LVGL already emits `LV_EVENT_GESTURE` (up/down/left/right) on any scrollable
or clickable object; none of it is wired up today.

| Idea | Effort | Why |
|---|---|---|
| **Swipe left/right between sibling screens** | **S** | Monitor ↔ Graph ↔ R-Test ↔ Battery as a horizontal deck. Removes most Menu trips; the Menu becomes a jump-to, not the only route. |
| **Swipe down = quick-settings shade** | S–M | Brightness, sample rate, load toggle, disconnect — the phone idiom, no screen switch. |
| **Edge-swipe right = back** | S | Frees the top-left corner, which the rounded glass clips anyway. |
| **Drag a hero value up/down to set it** | M | Coarse setpoint changes without ever opening Adjust. Drag distance → magnitude, with live preview and release-to-commit. |
| **Circular scrub around the bezel** | M | The panel is a rounded square — a circular drag is a natural fine-adjust dial, like a watch crown. Distinctive and genuinely ergonomic. |
| **Long-press Load = arm/confirm** | S | Deliberate action for high-power switch-on without a modal dialog. |
| **Double-tap a hero to cycle its metric** | S | V → W → Ah on the same block; two heroes then cover any 2 of 5 values. |
| **Pull-to-rescan on Connect** | S | Standard idiom, removes a button. |
| **Two-finger tap = emergency LOAD OFF** | S | Works from any screen, no aiming required. |

## 9. At-a-glance information design

Small screen, so every pixel should carry information.

| Idea | Effort | Why |
|---|---|---|
| **Sparkline behind each hero number** | **S–M** | A faint 60-sample trend drawn *behind* the digits. You get trend + value in the same space, and the Graph screen becomes optional rather than necessary. Probably the single best density win available. |
| **Progress arc around the screen edge** | M | Test progress as an arc tracing the rounded perimeter. Uses the form factor's most distinctive feature, and readable from across the bench. |
| **Power-envelope bar** | S | A thin bar showing draw as % of the EL15's 150 W / 12 A / 60 V envelope, so you see headroom before you hit a limit. |
| **Ghost target marker on the current hero** | S | Commanded setpoint drawn as a faint marker behind the actual reading — instantly shows regulation error / when the load can't reach setpoint. |
| **Min / max / avg capture with hold** | S | Live statistics on the Monitor, resettable. Standard DMM behaviour that's missing here. |
| **Delta (relative) mode** | S | Zero the reading, show change from that point. Invaluable for finding a bad connection by wiggling it. |
| **Trend arrows on slow-moving values** | S | Temperature and voltage drift direction at a glance. |
| ~~Time-remaining estimate during capacity tests~~ | **done** | Enter the pack's rated mAh and the run screen shows "% of rated drawn" plus an ETA to the rating at the present current; the result adds state of health and C-rate. |
| **Tappable telemetry bar** | S | Cycle the bar's contents (W/fan/temp/runtime → Wh/Ah/mΩ/cell-V) instead of it being fixed. |

## 10. Screen structure

| Idea | Effort | Why |
|---|---|---|
| ~~Persistent running-test chip on every screen~~ | **done** | Status-strip chip ("R-TEST 4/8", "BATT 01:23", "BATT PAUSED", "… result") on every screen but the test's own; tapping returns to the test. The old yank-you-back-on-every-progress behaviour is gone with it. |
| **Fold Adjust into Monitor** | M | With drag-to-set and an inline stepper, the Adjust screen could disappear entirely — one less place to navigate to. |
| **Split view: heroes + mini graph** | M | Alternative Monitor layout for people who want the trend permanently visible. |
| **Selectable home screen** | S | Let Monitor, Graph, or a test screen be the boot/home destination per workflow. |
| **Hide unused modes** | S | Most benches use 2–3 modes; let the picker be pruned so the grid isn't 8 tiles of mostly noise. |
| **Landscape layout** | M | With the IMU, a rotated layout could put V and I side by side with a wide graph. |

## 11. Visual identity & AMOLED-specific

| Idea | Effort | Why |
|---|---|---|
| ~~Burn-in mitigation (pixel shift)~~ | **done** | The whole layout creeps around a 3x3 px box every 90 s (`display.cpp`), on by default, toggle in Settings ▸ Screen protection. |
| ~~True-black idle mode~~ | **done** | Idle dim → blank (AMOLED black = no power); see the auto-dim row in §5. |
| **7-segment / LCD-style hero font** | M | A generated instrument-style numeric font would make it *look* like bench gear instead of a phone app. Also lets you reach the 64 px hero size the original spec wanted (Montserrat maxes at 48). |
| **Proper icon font (Phosphor)** | M | Replaces the approximated LVGL built-in symbols noted in the QA guide. |
| **Screen transitions (slide/fade)** | S | LVGL supports animated screen changes; makes swipe navigation feel real. |
| **Value count-up animation** | S | Numbers roll rather than jump — reads as smoother without slowing anything. |
| **Per-screen accent colour** | S | Orientation cue when swiping between screens. |
| **Restore the "pulsing SINKING" indicator** | S | Was in the v2 spec, currently static — a breathing animation on live current draw. |
| **Boot splash** | S | Identity, and covers the ~1 s panel bring-up. |

## 12. Alerting & feedback

| Idea | Effort | Why |
|---|---|---|
| **Full-screen fault takeover** | S | A protection trip currently shows a banner that can be scrolled past. A full-screen red state with the reason and a single "Acknowledge" is appropriate for something driving real current. |
| **Screen flash / colour pulse on trip** | S | Peripheral-vision alert when you're not looking at the device. |
| **Toasts instead of persistent status labels** | S | The R-test/battery error labels currently stick around forever once shown. |
| **Completion celebration state** | S | Big, unmissable "TEST COMPLETE" with the headline number, visible across the room. |

## 13. More feature ideas (non-UI)

| Idea | Effort | Notes |
|---|---|---|
| **Energy cost calculator** | S | Wh × $/kWh — meaningful for efficiency and runtime work. |
| **Battery cycle counter per pack ID** | M | Tracks how many discharges a given pack has seen; pairs with SoH trending. |
| **Automatic HTML/PDF test report** | M | Generate a shareable report with curve + results (pairs with SD or WiFi). |
| **Load-profile replay from CSV** | M | Drive a recorded real-world current profile from the SD card. |
| **Temperature compensation of readings** | M | Correct for load-cell drift as the unit heats. |
| **Test queue / chaining** | M | Run R-test → capacity test → rest, unattended, in one go. |
| **Guided wizard vs expert mode** | M | Wizard walks a newcomer through hooking up a pack safely; expert mode is today's UI. |
| **Multi-language** | M | LVGL handles it; needs font coverage for non-Latin scripts. |
| **"Repeat last test" one-tap** | S | Most tests get run several times in a row. |

## 14. Audio *(hardware confirmed and in use)*

**Resolved:** the board *does* carry an onboard codec — an **ES8311** at I²C
`0x18` on the shared bus, with the speaker amp's power-enable on **TCA9554
expander bit 7**. I²S MCLK=19 BCLK=20 DIN=21 WS=22 DOUT=23, all pinned in
`board_config.h` and driven by `audio.cpp` + the vendored `es8311.c`. So this is
true I²S audio: arbitrary tones today, samples if ever wanted.

**Already shipped:** startup chime, key-click on taps, firmer confirm on the
physical buttons, rising two-tone on test completion, falling two-tone on error,
urgent repeating alarm on a protection trip / emergency stop / link-guard event,
plus **volume + mute in Settings** (applied by scaling the synthesised PCV, so no
codec I²C traffic at runtime). Init failure is non-fatal — the calls no-op.

**One hard-won rule:** the I²S stream must run **continuously**. Stopping and
restarting it per tone made short tones "cut in and out"; a single continuous
stream is clean. Keep the one streaming task in `audio.cpp` (priority 8, above
the loop task).

Bench instruments live or die on their audio feedback: you are usually looking
at your probes and your hands, not at the screen. The rows below marked done are
shipped; the rest are still open.

| Idea | Effort | Why |
|---|---|---|
| ~~Test-complete tone~~ | **done** | Rising two-tone on completion of either engine (`audio::success()`). |
| ~~Distinct pass / fail tones~~ | **done** | Rising for complete, falling for error/abort. Still to pair with go/no-go limits (§2) for a real pass/fail verdict. |
| ~~Protection-trip alarm~~ | **done** | `audio::fault()` — urgent alarm on a protection trip, emergency stop, brownout auto-off, and every link-guard recovery attempt. |
| **Audible probe mode** | **M** | *The standout idea.* Map the live measurement to pitch — like a multimeter's continuity beep, but continuous and proportional. Probe a connection and **hear** the resistance/current change while watching your hands. Nothing else on this list improves hands-on work as much. |
| **"Load is ON" reminder chirp** | S | A soft periodic chirp whenever current is flowing, like a reversing beeper. Cheap insurance against walking away from an energised setup. |
| **Cutoff-approaching warning** | S | Escalating beeps as a discharge nears its cutoff voltage — signals the interesting part is about to happen. |
| **Countdown before high-power start** | S | Three beeps before the load engages above a threshold, with time to abort. |
| ~~Key-click / touch feedback~~ | **done** | `audio::click()` on taps, `audio::press()` on the physical buttons. |
| **Per-fault alarm patterns** | S | Different rhythms for REV vs UVP vs over-temp, so the fault is identifiable without looking. |
| ~~Startup chime~~ | **done** | Played in `setup()` — doubles as proof the codec came up. |
| ~~Volume + mute in Settings~~ | **done** | Settings ▸ System, persisted in NVS. A "silent after 22:00" / "alerts only" mode is still open. |
| **Spoken results** | L | Pre-recorded samples on SD ("test complete, two point one amp hours"). Genuinely useful when your hands are full — but the heaviest item here, and the only one that needs a real DAC/amp rather than a piezo. |

## 15. Physical buttons *(PWR + BOOT, both now wired up)*

Two buttons you can hit **without looking at the screen** — which is exactly
what you want when something is wrong. BOOT is GPIO9 (active-low strapping pin,
safe to read at runtime); PWR is not a GPIO at all — its presses arrive as
**AXP2101 IRQ status bits** (INTSTS2 `0x49`, bits 3 = short / 2 = long).

| Idea | Effort | Why |
|---|---|---|
| ~~Hardware emergency stop~~ | **done** | **BOOT** kills the load from any screen or state: stops both engines, pushes LOAD OFF + setpoint 0, alarm, red ack banner — and says so honestly if it could not reach the load. |
| ~~Wake / sleep toggle~~ | **done** | **PWR short** toggles true-black display sleep (touch inert while asleep); a blind press while asleep only wakes. |
| ~~Load-safe power-off~~ | **done** | **PWR long** stops the load and flushes the write *before* cutting the rails — intercepting the PMIC's own OFFLEVEL cutoff, which would strand an energised load. |
| **Start / stop the current test** | S | Eyes-free control while your hands are on the probes. Needs a third gesture (double-press?) — both button actions are taken. |
| **Long-press for a screenshot** | S | Documentation without touching the UI you want to capture. Now that SD works, this has somewhere to write to. |
| **Hold at boot for safe mode** | S | Skip auto-connect / reset settings if a config ever wedges startup. More useful now that auto-connect exists. |

---

## Suggested next moves

*Landed since this list was written: SD CSV reports (now on their own
bit-banged SPI pins and hardware-verified), NVS persistence, the link-loss
supervisor + crash recovery, controller-brownout auto-safe and load-safe
power-off, NTP clock sync, AMOLED burn-in mitigation, 4-wire/tare R-test
support, ES8311 audio feedback, both physical buttons, and startup
auto-connect. What's left, highest value first:*

0. **Validate what's already built with real current** — every safety layer
   above (link guard, crash recovery, brownout auto-off) is compile-clean and
   never once fired for real. This outranks every new feature below.

1. **Solar I-V / max-power-point mode** — the sweep engine already collects
   everything needed; this is mostly a second results view. (S)
2. **Named test profiles / history** — NVS persistence landed, but save/recall
   of *named* setups and an on-device history browser (now that SD works) are
   the next step. (M)
3. **OTA over WiFi** — the radio is already used for NTP; OTA removes the USB
   cable for updates. (S–M)
4. ~~Capacity per-sample CSV~~ **done** — `sample_log.{h,cpp}` buffers
   datapoints in flash during the run and `BATT_NNN.CSV` now carries the full
   curve (V/I/P/mAh/Wh/temp per sample).

If the goal is **turning this into a product-grade instrument**: calibration,
pass/fail limits, named history, and the web UI are the differentiators.

If the goal is **making the device nicer to actually use**, in order:

1. **Swipe navigation between screens** — removes most Menu trips immediately. (S)
2. **Sparklines behind the hero numbers** — trend + value in the same pixels. (S–M)
3. **Persistent running-test chip** — stop the UI hijacking the screen during
   a test. (S–M)
4. **Full-screen fault takeover** — the alerting is currently too easy to miss
   for something driving real current. (S)

*(AMOLED burn-in pixel shift is done — see §11.)*

---

*Companion docs: CAPACITY_PLAN.md (battery test roadmap), QA_GUIDE.md (test
matrix), QA_REPORT.md (open defects).*
