# EL15 Controller — UI Redesign Brief

A design brief for a **new touchscreen UI** for a standalone hardware controller.
This document is self-contained: you do not need the source code to design from
it. The goal is a layout that is **legible and tappable on a very small screen**.

---

## 1. What the device is

A small, self-contained **benchtop instrument** that remotely controls an
**ALIENTEK EL15 electronic load** (a "DC load tester" — a box that sinks a
programmable amount of current so you can test batteries, power supplies, and
wiring). The controller talks to the EL15 over Bluetooth Low Energy; **no phone
or PC is involved** — this device *is* the interface.

**Who uses it and where:** a technician or hobbyist at a workbench, often with
both hands busy on wiring/probes, glancing at the screen from ~30–50 cm away.
It drives **real power hardware**, so the live readout and the load ON/OFF and
setpoint controls are safety-relevant — they must be unambiguous and hard to
mis-tap.

**Core jobs the UI must support (v1):**
1. **Connect** to the load (pick from a scan list, or a built-in demo simulator).
2. **Monitor** live measurements (voltage, current, power, temperature, etc.).
3. **Control** the load: choose a mode, enter a numeric setpoint, turn the load
   on/off, lock the load's own keypad.
4. **Run a circuit-resistance test**: enter a fuse rating, start a stepped
   current sweep, watch progress, read the result.

---

## 2. Hardware & screen specs

| Spec | Value |
|---|---|
| Board | Waveshare ESP32-C6-Touch-AMOLED-1.8 |
| MCU | ESP32-C6 (RISC-V, ~160 MHz), 320 KB RAM, 16 MB flash |
| Display | 1.8" **AMOLED**, SH8601 controller |
| **Resolution** | **368 × 448 px, portrait** (taller than wide) |
| Pixel density | **≈ 322 ppi ≈ 12.7 px/mm** |
| **Physical active area** | **≈ 29 mm wide × 35 mm tall** (about 1.14" × 1.39") |
| Color | 16-bit RGB565 (65k colors), no true 24-bit |
| Brightness | Software-controllable 0–255 (global) |
| Input | **Capacitive touch only** — no physical buttons, no rotary encoder, no stylus. Single-finger. |
| Rendering toolkit | **LVGL 8.4** (embedded C UI library) |

### What "AMOLED" means for design
- **True black = pixels off** → infinite contrast and lower power on dark UIs. A
  **dark theme is the native, correct choice** (also matches the instrument
  aesthetic below). Avoid large bright/white fills — they cost power and glare.
- Per-pixel emission → **bright accent colors pop** against black. Good for a few
  saturated status colors (green/amber/red) on a near-black field.
- No backlight zones; brightness is global.

---

## 3. The core constraint: this screen is TINY

At ~29 × 35 mm, the whole display is roughly the size of a thumbnail. Translate
touch ergonomics into pixels:

- Recommended **minimum** finger target ≈ 9 mm ≈ **~115 px**.
- **Comfortable** target ≈ 10–11 mm ≈ **~127–140 px**.
- ⇒ The entire screen is only about a **3-column × 3–4-row grid** of
  finger-sized targets. **368 px wide fits ~3 comfortable buttons across; 448 px
  tall fits ~3–4 down.** Budget accordingly.

Legibility (viewed at ~30–50 cm):
- Body/label text: **≥ 16 px**, ideally 18–22 px. Avoid anything below 14 px.
- The **hero live readout** (the current voltage/current) should be **large —
  ~48–96 px tall digits** — readable at a glance across the bench.
- Monospaced/tabular figures for numbers that change rapidly (so they don't
  jitter in width).

Implications the redesign should embrace:
- **Do fewer things per screen.** The current design crams a full instrument
  panel + tabs + text fields onto one view; it's unreadable. Prefer a small
  number of focused screens/cards with big elements.
- **No tiny tab bars or dense chip rows.** A 6–7 item chip selector and a 3-tab
  bar at this size are too small to hit reliably.
- **Numeric entry is a real problem.** There is no keyboard. A full QWERTY/number
  pad barely fits and its keys are sub-target-size. Prefer **large +/- steppers,
  presets, or a big number pad that takes the whole screen** when active.

---

## 4. Functional requirements — every screen & control

The redesign may reorganize these freely, but must not drop functionality.

### 4.1 Connection
- Trigger a **scan**; show a **list of found devices** (name + address).
  Tapping one connects. (There is intentionally no on-device demo entry —
  hardware-free testing uses the Android EL15 Load Simulator app over real BLE.)
- Show **connection state** persistently: disconnected / scanning / connecting /
  connected (+ a short status string).
- A **Connect / Disconnect** affordance.
- (Design note: scan results can be many and repeat — the list needs to handle a
  handful of entries with big, tappable rows.)

### 4.2 Monitor (the live readout — the hero of the product)
Continuously updated ~2×/sec while connected:
- **Voltage (V)** and **Current (A)** — the two primary values. These are the
  hero numbers.
- **Power (W)** — derived.
- **Active mode** label (CC / CV / CR / CP / CAP / DCR — see below).
- **Temperature (°C)** of the load, **fan speed** (0–5), **load ON/OFF** state,
  **ready/idle** state, and **runtime**.
- **Protection / fault banner** when the load trips (e.g. REV = reverse polarity,
  UVP = under-voltage) — must be **loud and obvious** (this is a fault state).
- Mode-specific extras: **CAP** mode shows accumulated energy (Wh) & capacity
  (Ah); **DCR** mode shows a measured milliohm value.

### 4.3 Control
- **Mode selector** — one of six: **CC** (constant current), **CV** (constant
  voltage), **CR** (constant resistance), **CP** (constant power), **CAP**
  (capacity), **DCR** (DC internal resistance). Only one active at a time.
- **Setpoint entry** — a single numeric value whose **label and unit follow the
  mode** (CC/CAP → Amps, CV → Volts, CR → Ohms, CP → Watts). Needs a way to type
  or dial in a value with a decimal. Range is modest (e.g. 0–12 A, 0–60 V).
- **Load ON / OFF** — a big, unmistakable toggle. Its state must reflect the
  *actual* load state reported by the hardware (not just what was tapped). This
  is the most safety-critical control — turning the load on drives real current.
- **Lock keypad** — disables the physical buttons on the EL15 itself.

### 4.4 Circuit-Resistance Test (R-Test)
Measures a circuit's series resistance by sweeping current and fitting a line.
- **Inputs:** fuse rating in amps (required, numeric), number of steps (integer,
  default 8).
- **Start / Stop** control.
- **Progress** while running: current step / total, and the live V & I per step,
  plus a progress bar.
- **Result:** computed resistance **R** (auto Ω / mΩ), open-circuit voltage
  **Voc**, and a fit-quality **R²** with a "low confidence" flag when poor.
- Safety: it commands real current ramps; the running state should be clearly
  distinct from idle, and stopping must be easy.

### 4.5 Future features (NOT in v1, but design the navigation to accommodate)
The companion phone app also has: four bench tests (battery capacity, runtime,
load-step, over-current-protection), on-device test **history** with graphs,
**settings** (theme, poll rate, brightness), and alarms. The new UI's navigation
model should leave **room to grow** to ~6–8 top-level destinations without
becoming a cramped tab strip.

---

## 5. Visual language

The existing aesthetic is a dark **"instrument-panel / blueprint"** look, and it
suits an AMOLED bench tool. Keep the spirit, improve the legibility.

- **Theme:** dark. Near-black background (true black regions are free on AMOLED).
- **Accent:** a single steel blue `#7BA1C9` for structure/headings/primary UI.
- **Status colors:** green `#4CAF50` (voltage / OK / load-safe), amber `#FFB300`
  (current), red `#EF5350` (fault / load-ON warning / stop). Use saturated
  accents sparingly against the dark field.
- **Ink:** `#E6EDF3` primary text, `#8B98A5` muted/secondary.
- **Surfaces:** slightly-raised cards `#121A23` with hairline borders
  (`#2A3441`).
- Condensed headings; **monospace for live numeric readouts**.
- The current palette is a good starting point — you may refine it, but keep the
  dark + steel + green/amber/red instrument identity.

---

## 6. Technical constraints for the designer

The design is implemented in **LVGL 8.4 in C++** on the device — not web/CSS. Aim
your layout at what LVGL does well, and flag anything custom so we can budget it.

- **Available building blocks** (cheap, built-in): labels, buttons, **arc gauges**,
  **bars/sliders**, lists, tabview, textareas, **on-screen keyboard**, **roller**
  (scroll-to-select), **spinbox**, checkboxes/switches, message boxes, flex/grid
  layout. Custom drawing (canvas) is possible but costs effort.
- **Fonts:** Montserrat is bundled at 12/14/16/20/28 px; **more sizes or a custom
  font can be added** (flash is ample at 16 MB) — if you want big 64–96 px
  digits, say so and we'll embed that size.
- **Performance:** the panel refreshes via **partial buffers (1/8 frame)**, so
  **flat fills, simple shapes, arcs, and bars are fast**; avoid large per-frame
  gradients, blurs, full-screen animations, or heavy image assets. Static icons
  are fine. Keep motion minimal and purposeful.
- **Color depth:** RGB565 — subtle gradients band; prefer solid fills and clear
  color steps.
- Single-finger touch only; no hover, no gestures assumed (simple taps + vertical
  scroll are safest). Long-press/swipe are possible but should not be the *only*
  way to reach anything.

---

## 7. What we're optimizing for (priorities)

1. **Glanceable live readout.** From across the bench, the user should instantly
   read voltage, current, power, mode, and whether the load is ON. This is the
   product's primary job — give it the most space and the biggest type.
2. **Unmistakable, hard-to-mis-tap load control.** Load ON/OFF and setpoint are
   safety-relevant. Big targets, clear state, obvious ON warning.
3. **Usable numeric entry without a keyboard.** Solve setpoint / fuse-rating /
   steps entry in a way that works with a fingertip on a 29 mm-wide screen.
4. **Legibility over density.** Fewer, larger elements per screen. It's fine to
   split things across screens or use a full-screen mode for a single task.
5. **Room to grow** to the future features in §4.5.

---

## 8. Deliverables requested

For each screen/state, please provide:
- A **layout** at the true aspect ratio (**368 × 448, portrait**) — annotated
  with element sizes in px where it matters for tappability.
- A **navigation model**: how the user moves between Monitor, Control, R-Test,
  Connect (and future destinations) with big touch targets — not a tiny tab bar.
- A proposed **numeric-entry pattern** (stepper / big keypad / roller / preset).
- **Component specs**: type sizes, the gauge/readout treatment, button sizes,
  color usage, and the fault/warning treatment.
- Notes on any **custom fonts or drawing** you're assuming so we can implement it
  in LVGL.

### Open questions for the designer to resolve
- Is a **single scrolling "home" screen** (hero readout on top, controls below)
  better than separate Monitor/Control screens on a portrait panel this small?
- Best **numeric entry** for a 29 mm-wide screen: full-screen number pad, big
  ± steppers with coarse/fine, or a roller per digit?
- How to present **mode selection** (6 modes) without a tiny chip row — a
  full-screen picker? A roller? Grouped large buttons?
- How to make **Load ON** state impossible to miss (color, size, persistent
  banner) while keeping the OFF path always one tap away.

---

## Appendix — reference numbers
- Screen: 368 × 448 px · 1.8" · ~322 ppi · ~12.7 px/mm · ~29 × 35 mm active.
- 1 mm ≈ 12.7 px · min finger target ≈ 115 px · comfortable ≈ 127–140 px.
- Whole screen ≈ a 3 × 3–4 grid of finger-sized targets.
- Modes: CC, CV, CR, CP, CAP, DCR. Ratings ceiling: 12 A / 150 W / 60 V.
