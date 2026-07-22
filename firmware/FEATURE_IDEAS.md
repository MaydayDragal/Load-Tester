# EL15 Controller — Feature Backlog

Candidate features for the ESP32-C6 controller. Effort: **S** = hours, **M** = a
session, **L** = multi-session. "Free" = needs no hardware you don't already have.

---

## 0. Hardware you already own but don't use yet

This is the headline: most of the board is idle. Every feature below marked
*(free)* rides on silicon already soldered down.

| Hardware | Status | What it unlocks |
|---|---|---|
| **WiFi 6 radio** | completely unused | web UI, REST/MQTT, NTP clock, OTA updates, cloud logging, alerts |
| **802.15.4 radio** (Thread/Zigbee/Matter) | completely unused | smart-home integration without WiFi |
| **QMI8658 6-axis IMU** | completely unused | auto-rotate, tap/shake gestures, wake-on-lift, e-stop shake |
| **SD card slot** | wired, driver stubbed | all logging/export (already Phase 0 of CAPACITY_PLAN.md) |
| **PCF85063 RTC** | read-only | real timestamps on logs — needs a set-time path (or NTP) |
| **AXP2101 PMIC** | read-only | sleep/low-power modes, runtime estimate, charge control |
| **PWR + BOOT buttons** | completely unused | physical emergency-stop / wake / quick-action keys that work without looking |
| **Audio out** | completely unused | completion tones, fault alarms, audible probing — see §14 for the hardware caveat |
| **USB-Serial/JTAG** | flashing + logs only | SCPI-style scripting API for lab automation |
| **RTC backup-battery pads** | unpopulated | keeps the clock running through a main-battery swap |

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
| **State-of-health tracking** | M | Compare measured Ah to rated Ah; trend across saved tests per battery ID. Needs SD/NVS history. |
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
| **NTP time sync** | **S** | Fixes the "RTC not set" problem with no UI work at all. |
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
| **Link-loss auto-stop supervisor** | **S–M** | *Real current gap today*: if BLE drops mid-discharge the load keeps sinking until the EL15's own UVP. Add reconnect-and-stop + a loud alert. |
| **Crash/reboot recovery** | M | NVS "test in progress" flag → on boot, reconnect and force LOAD OFF. |
| **Temperature auto-abort** | S | User-set limit; stop before the load cooks. |
| **User soft limits** | S | Cap max current/power below hardware ratings for a given setup. |
| **Confirm dialog above a power threshold** | S | Guard rail for high-energy runs. |
| **Emergency stop gesture** | S | Shake (IMU) or long-press anywhere → LOAD OFF. |
| **Audible alert** | S | Trip/completion tone — the board has an I²S audio output, so this is free. See §14. |
| **Screen lock during a test** | S | Stops accidental taps changing a running test. |

## 5. UI / UX

| Feature | Effort | Notes |
|---|---|---|
| **Persistent settings + test profiles (NVS)** | **S–M** | Remember brightness, sample rate, last battery config; save/recall named setups. Highest quality-of-life per hour of work. |
| **Auto-dim / sleep + wake-on-tap** | S | Battery life during long unattended tests. |
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
| **Zero/tare offset** | S | Removes lead/contact offset before an R-test. |
| **4-wire (Kelvin) workflow support** | S | Mostly UI guidance + a mode flag; dramatically improves low-resistance accuracy. |
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
| **Time-remaining estimate during capacity tests** | S | Currently only elapsed is shown; ETA is what you actually want when a test runs for hours. |
| **Tappable telemetry bar** | S | Cycle the bar's contents (W/fan/temp/runtime → Wh/Ah/mΩ/cell-V) instead of it being fixed. |

## 10. Screen structure

| Idea | Effort | Why |
|---|---|---|
| **Persistent running-test chip on every screen** | **S–M** | Today a running R-test yanks you back to its screen on every progress callback. A small chip ("R-Test 4/8 ▸") in the chrome would let you browse freely while a test runs and tap to return. Fixes a real annoyance. |
| **Fold Adjust into Monitor** | M | With drag-to-set and an inline stepper, the Adjust screen could disappear entirely — one less place to navigate to. |
| **Split view: heroes + mini graph** | M | Alternative Monitor layout for people who want the trend permanently visible. |
| **Selectable home screen** | S | Let Monitor, Graph, or a test screen be the boot/home destination per workflow. |
| **Hide unused modes** | S | Most benches use 2–3 modes; let the picker be pruned so the grid isn't 8 tiles of mostly noise. |
| **Landscape layout** | M | With the IMU, a rotated layout could put V and I side by side with a wide graph. |

## 11. Visual identity & AMOLED-specific

| Idea | Effort | Why |
|---|---|---|
| **Burn-in mitigation (pixel shift)** | **S** | *Genuinely important and non-obvious*: this is a static instrument UI on an **AMOLED**. Fixed labels like "VOLTAGE"/"CURRENT" sitting in place for hours will burn in permanently. A 1–2 px slow drift of the whole layout, plus periodic full-screen refresh, costs almost nothing and protects the panel. |
| **True-black idle mode** | S | AMOLED pixels use no power when black. Dim to a single value on a black field when idle — big battery win on long unattended tests. |
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

## 14. Audio *(currently unused)*

**Hardware check needed first.** Waveshare's published spec for this board does
not list an audio codec, amplifier, or speaker — so confirm which of these you
have before building on it:

- an **onboard codec/amp** (some Waveshare AMOLED variants ship one) — then
  it's true I²S audio, arbitrary tones and even samples;
- an **external I²S DAC/amp** on the exposed pads — same capability, one module;
- **a piezo or small speaker on a spare GPIO**, driven by the C6's LEDC/PWM —
  no extra chip, plenty for tones and beeps, which covers everything below
  except spoken output.

Either way, nothing in `firmware/src` touches audio today and `board_config.h`
has no audio pins yet — pin them down against Waveshare's `pin_config.h` first
(same verification caveat as the SD card's CS pin).

Bench instruments live or die on their audio feedback: you are usually looking
at your probes and your hands, not at the screen.

| Idea | Effort | Why |
|---|---|---|
| **Test-complete tone** | **S** | The original ask. A capacity test runs for hours — you want to be told, from the next room, that it finished. |
| **Distinct pass / fail tones** | S | Rising two-tone for pass, falling buzz for fail. Pairs with go/no-go limits (§2); makes production-style testing eyes-free. |
| **Protection-trip alarm** | **S** | Urgent, loud, repeating until acknowledged. This is the most safety-relevant use: something driving real current tripped, and a silent red banner is easy to miss. |
| **Audible probe mode** | **M** | *The standout idea.* Map the live measurement to pitch — like a multimeter's continuity beep, but continuous and proportional. Probe a connection and **hear** the resistance/current change while watching your hands. Nothing else on this list improves hands-on work as much. |
| **"Load is ON" reminder chirp** | S | A soft periodic chirp whenever current is flowing, like a reversing beeper. Cheap insurance against walking away from an energised setup. |
| **Cutoff-approaching warning** | S | Escalating beeps as a discharge nears its cutoff voltage — signals the interesting part is about to happen. |
| **Countdown before high-power start** | S | Three beeps before the load engages above a threshold, with time to abort. |
| **Key-click / touch feedback** | S | Audible confirmation that a tap registered — genuinely useful given the touch-responsiveness history on this panel. |
| **Per-fault alarm patterns** | S | Different rhythms for REV vs UVP vs over-temp, so the fault is identifiable without looking. |
| **Startup chime** | S | Confirms boot and that audio works. |
| **Volume + mute in Settings** | **S** | Mandatory companion to all of the above — a bench tool that can't be silenced becomes a bench tool people unplug. Include a "silent after 22:00" or "alerts only" mode if you like. |
| **Spoken results** | L | Pre-recorded samples on SD ("test complete, two point one amp hours"). Genuinely useful when your hands are full — but the heaviest item here, and the only one that needs a real DAC/amp rather than a piezo. |

## 15. Physical buttons *(PWR + BOOT, currently unused)*

Two programmable buttons you can hit **without looking at the screen** — which
is exactly what you want when something is wrong.

| Idea | Effort | Why |
|---|---|---|
| **Hardware emergency stop** | **S** | One button = LOAD OFF from any screen, any state, no aiming at a touch target. The most defensible use of a physical key on a device driving real current. |
| **Wake / sleep toggle** | S | Pairs with auto-dim and the true-black idle mode. |
| **Start / stop the current test** | S | Eyes-free control while your hands are on the probes. |
| **Long-press for a screenshot** | S | Documentation without touching the UI you want to capture. |
| **Hold at boot for safe mode** | S | Skip auto-connect / reset settings if a config ever wedges startup. |

---

## Suggested next moves

If the goal is **maximum value for least work**, in order:

1. **Solar I-V / max-power-point mode** — the sweep engine already collects
   everything needed; this is mostly a second results view. (S)
2. **NVS persistence + test profiles** — every session currently starts from
   defaults; this is the most-felt daily annoyance. (S–M)
3. **SD logging** — unblocks history, comparison, and export all at once. (M)
4. **Link-loss auto-stop supervisor** — closes the one genuine safety gap in
   the current design. (S–M)
5. **NTP + OTA over WiFi** — real timestamps and cable-free updates, and it
   lights up a radio that is doing nothing today. (S–M)

If the goal is **turning this into a product-grade instrument**: calibration,
pass/fail limits, named history, and the web UI are the differentiators.

If the goal is **making the device nicer to actually use**, in order:

1. **Swipe navigation between screens** — removes most Menu trips immediately. (S)
2. **Sparklines behind the hero numbers** — trend + value in the same pixels. (S–M)
3. **AMOLED burn-in pixel shift** — do this before the panel is damaged; it is
   cheap now and unfixable later. (S)
4. **Persistent running-test chip** — stop the UI hijacking the screen during
   a test. (S–M)
5. **Full-screen fault takeover** — the alerting is currently too easy to miss
   for something driving real current. (S)

---

*Companion docs: CAPACITY_PLAN.md (battery test roadmap), QA_GUIDE.md (test
matrix), QA_REPORT.md (open defects).*
