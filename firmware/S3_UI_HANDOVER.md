# UI handover — the 3.5″ (ESP32-S3) controller

For the designer taking on the interface of the **ESP32-S3-Touch-LCD-3.5** build
of the EL15 load controller.

You do not need to read code. You do need [`UI_DESIGN_BRIEF.md`](UI_DESIGN_BRIEF.md)
first — it explains what the device *is*, who uses it, and the visual language
the current UI speaks. **This document is the delta**: everything that changes
because the hardware changed, plus the constraints you will design against.

Two warnings before you start:

1. **The brief you are reading alongside this describes a different panel.** It
   was written for the 1.8″ AMOLED board, and its central argument — "the core
   constraint: this screen is TINY" — **no longer holds**. That constraint is
   what shaped most of the current layout. It is gone. Read §1 below before you
   take any of that brief's sizing advice literally.
2. **Nobody has seen this UI on the new panel.** The firmware compiles for the
   S3 board and every subsystem has a code path, but the hardware was not in
   hand when the port was written and has never been powered on. Anything below
   marked *unverified* is an inference from the vendor's documentation, not an
   observation. See [`S3_BRINGUP.md`](S3_BRINGUP.md).

---

## 1. The one thing that changes everything

| | 1.8″ AMOLED (old) | 3.5″ IPS (new) | Change |
|---|---|---|---|
| Resolution | 368 × 448 | **320 × 480** | −48 px wide, +32 px tall |
| Total pixels | 164,864 | 153,600 | **−7 %** |
| Physical active area | 29.0 × 35.3 mm | **49.3 × 74.0 mm** | **3.6 × the area** |
| Pixel density | 322 ppi (12.7 px/mm) | **165 ppi (6.5 px/mm)** | **half** |
| Panel type | AMOLED | IPS LCD | see §3 |

Read that table twice, because the intuition it produces is the opposite of the
usual one. **You are not getting a bigger canvas to fill with more stuff.** You
are getting *slightly fewer pixels*, spread over *three and a half times the
glass*. One pixel now covers about **twice** the physical distance it did.

The practical consequence:

> Every pixel dimension in the current design becomes **roughly twice as large
> physically**, at no cost in pixels.

The old design was in a constant fight with physical smallness. A 42 px status
bar was **3.3 mm** tall — barely a fingertip. Body text at 16 px was **1.26 mm**
— genuinely squinty at arm's length. The layout is full of compensations for
this: oversized tap targets, invisible touch-area extensions, a 40 px
"touch-snap" radius that quietly redirects near-misses onto the nearest control.

On the new panel those same numbers are comfortable. That 42 px bar is
**6.5 mm**. That 16 px text is **2.5 mm**. The fight is over.

**So the design question flips.** It is no longer "how do I make this legible at
all?" It is: **"what do I do with the physical room I just gained, given that I
have 48 fewer pixels across to do it in?"**

Two honest ways to spend it, and you should decide deliberately:

- **Spend it on calm.** Keep roughly the current pixel metrics. The result is
  the same information, far more legible and tappable, with real breathing room.
  Lowest risk, and a large usability win on its own.
- **Spend it on density.** Shrink pixel metrics (a 34 px bar is still 5.2 mm)
  and use the freed pixels for more content per screen — fewer trips into the
  menu, more of the instrument visible at once.

My recommendation is **mostly the first, selectively the second**: keep type and
touch targets generous, but reclaim pixels from *chrome* (bars, paddings,
insets) where physical size was never the point.

### The 320 px width is the new hard edge

This is the one place you are genuinely worse off. Anything currently tuned to
368 px must be re-flowed, and the failure mode is silent: LVGL's flex layout
**wraps** rather than clipping, so an over-wide row quietly becomes two rows and
the layout goes soft rather than visibly breaking.

One known instance is already fixed: the 2-column menu grid used fixed 164 px
tiles (2 × 164 = 328 > 320) and now derives tile width from the panel. Assume
there are others that only show up on real glass.

### Sizing reference for the new panel

| Intent | Physical | New panel px |
|---|---|---|
| Absolute minimum tap target | 7 mm | **46 px** |
| Comfortable tap target | 9 mm | **58 px** |
| Generous / safety-critical target | 12 mm | **78 px** |
| Caption text | ~1.9 mm em | 12 px |
| Body text | ~2.5 mm em | 16 px |
| Section heading | ~3 mm em | 20 px |
| Hero readout (voltage/current) | ~5–7.5 mm em | 34–48 px |

Fonts available today are Montserrat at **12, 14, 16, 20, 24, 28, 34, 40, 44,
48 px**. Those are bitmap fonts compiled into the firmware — any other size, or
any other typeface, must be converted and costs flash. 48 px is currently the
largest; if the hero readouts want to be physically bigger than ~7.4 mm, say so
and a larger size can be added.

---

## 2. What you must not break

This device drives real power hardware — it sinks current from batteries and
supplies. Some of the UI is safety-relevant, and those parts have already been
through failure analysis. Treat them as fixed requirements, not suggestions.

- **The load ON/OFF control must be unambiguous and hard to mis-tap.** It shows
  the *hardware's* actual state, reported back by the load, never the state the
  user just tapped. Keep those distinct.
- **A running test must be visible and reachable from every screen.** There is a
  persistent "running test" chip in the status bar for exactly this reason:
  walking away from a running test used to leave no obvious way back, and the
  test looked lost. Do not let a redesign strand it.
- **Alerts about the load are not dismissible decoration.** Link-loss, emergency
  stop and brownout warnings force the screen awake and take over. They must
  outrank whatever else is on screen.
- **Colour must never be the only signal.** Green/amber/red carry status here,
  but a shape, glyph or word has to carry it too.
- **Destructive or energising actions need deliberate targets.** Bigger, further
  from the edge, further from their opposite.

---

## 3. IPS is not AMOLED — five things that change

1. **Black is no longer free.** On the old panel, black pixels were *off*:
   infinite contrast, zero power. This panel is backlit, so "black" is dark grey
   lit from behind, and a black UI costs exactly as much power as a white one.
   A dark theme is still right for a bench instrument, but choose it for the
   aesthetic, not for the power or the contrast.
2. **Your darkest greys will collapse.** The current palette leans on very close
   near-blacks — `#000000`, `#0A0E13`, `#0D141B`, `#121A23` — which separate
   cleanly on AMOLED and may become one indistinguishable murk on IPS. **Expect
   to re-space the dark end of the ramp with more contrast between steps.** This
   is the most likely purely-visual regression, and it needs real glass to
   judge.
3. **Burn-in is no longer a threat.** The old panel needed active mitigation: the
   whole UI crept around a 3 × 3 px box every 90 seconds to stop static elements
   etching in. That is disabled here. You may design genuinely static furniture
   without guilt.
4. **Viewing angle is better; peak brightness is lower.** IPS holds colour off-
   axis, which suits a bench instrument glanced at from the side. It will not
   punch through direct sunlight the way an AMOLED can.
5. **Still 16-bit colour (RGB565).** ~65k colours, not 16.7M. Wide, subtle
   gradients will band visibly. Prefer flat fills and small, high-contrast
   accents.

---

## 4. The performance budget — please read this one

The new board has a faster processor and 8 MB of PSRAM, so the natural
assumption is that the UI got faster. **For pixel-pushing, it got slower.** The
old panel used a 4-lane bus; this one uses a single-lane SPI.

| | Old (QSPI ×4 @ 80 MHz) | New (SPI ×1 @ 40 MHz) |
|---|---|---|
| Full-screen redraw | **~8 ms** | **~61 ms** |

That is roughly **8× slower**, and it is a hard bandwidth limit, not something
optimisation can remove. (Raising the bus to 80 MHz — which the vendor's own
code does — would halve it to ~31 ms. That is an early bring-up experiment, not
something to design around yet.)

What this means for you:

- **Avoid full-screen transitions.** A slide or cross-fade between screens
  repaints everything, repeatedly. At ~61 ms a frame it will look like a stutter,
  not a transition. Prefer instant screen changes, or animate a small region.
- **Partial updates are cheap; full repaints are not.** A design where a live
  value updates inside a small fixed box costs almost nothing. A design where a
  changing value reflows its neighbours forces a wide repaint several times a
  second.
- **Give live readouts fixed-width space.** If a number's width changes with its
  value, its container reflows and the repaint spreads outward. This matters
  more here than it did before.
- **Large moving backgrounds, animated gradients and shadow-heavy cards are
  expensive.** Shadows in particular are costly to render.

Live data arrives at up to ~19 samples/second, the display refreshes on a 16 ms
tick, and touch is sampled every 10 ms.

---

## 5. What exists today

Seven screens, reached from an 8-tile menu overlay:

| Screen | What it is |
|---|---|
| **Monitor** | The home screen. Live voltage/current heroes, telemetry row (power, fan, temperature, runtime), mode + setpoint bar, pinned load/run-test bar. |
| **Adjust** | Setting the load's target value. Dial-stepper with hold-to-repeat, plus a numeric keypad. |
| **Graph** | Live two-series auto-scaling voltage/current chart. |
| **R-Test** | Circuit-resistance test: setup, live sweep with graphs, then a result page with an uncertainty figure. |
| **Battery** | Capacity/discharge test: chemistry and cell-count setup, live discharge curve, time-to-cutoff estimate, result. |
| **Connect** | Bluetooth scan list, connect/disconnect. |
| **Settings** | Sample rate, probe wiring, brightness, volume, screen timeout, clock/Wi-Fi, SD card, battery, system info. |

Five overlays sit above any screen: **menu**, **numeric keypad**, **picker**,
**text entry**, **Wi-Fi network list**.

Persistent chrome, present on every screen:

- **Status strip** (42 px tall today): connection dot + label on the left; on the
  right, the running-test chip, the controller's battery percentage, and the
  menu button. Also hosts the Back control on non-Monitor screens.
- **Info bar** and **load bar**, height-to-content.
- A **full-width alert banner** that overlays everything for safety events.

### Current design tokens

Backgrounds run `#000000` → `#0A0E13` (chrome) → `#121A23` (card) →
`#0D141B` (inset) → `#0B1016` (readout). Borders `#2A3441` / `#1C2530` /
`#14181F`. Text `#E6EDF3` (primary), `#8B98A5` (muted), `#5C6672` (faint).
Accent `#9184D9` with `#C9C2F2` as its light pair. Status: green `#4CAF50`,
amber `#FFB300`, red `#EF5350`. The voltage and current heroes each have their
own tinted background/border pair (green-black and amber-black).

Take these as the current state, not as a constraint — but see §3.2 on the dark
end.

---

## 6. What is buildable

The UI is built in **LVGL 8.4**, an embedded C toolkit. It is more capable than
you might expect and less capable than a browser. Designing to it saves a
painful round of "we can't build that".

**Available and cheap:** rectangles with corner radii, borders, flat fills,
opacity, bitmap text with anti-aliasing, flexbox and grid layouts, buttons,
button matrices, charts (line/scatter, multi-series), sliders, arcs, bars,
switches, rollers, tables, lists, text areas, message boxes, scrolling
containers.

**Available but costly:** shadows, simple linear gradients (vertical or
horizontal only), large blurred or translucent overlays.

**Not available:** SVG or any vector rendering, arbitrary rotation, blur
effects, per-glyph typographic control, web fonts, CSS-like cascading.

**Icons** come from LVGL's built-in symbol font — a small FontAwesome subset
(battery levels, charge bolt, Bluetooth, settings gear, arrows, list, edit, eye,
loop, shuffle, and a few dozen more). If you need iconography beyond that set,
it must be supplied as bitmaps.

**And here is a genuine new capability:** the old board had no spare memory for
image assets, which is why the current UI is drawn almost entirely from
rectangles and font glyphs. **This board has 8 MB of PSRAM.** Custom bitmap
icons, small illustrations and richer graphics are now affordable. If the design
wants real iconography instead of repurposed symbol glyphs, that is now on the
table — flag it and we will size the flash cost.

---

## 7. Touch

Capacitive, **single-finger only**. No multi-touch, no gestures beyond press and
drag, no hover, no stylus, no physical rotary control. There are two hardware
buttons: **BOOT** (emergency stop) and **PWR** (sleep / power-off).

Two behaviours worth knowing because they affect how forgiving your layout needs
to be:

- **Touch-snap.** A press that lands within 40 px of a control but not on it is
  redirected onto that control. On the new panel 40 px is ~6.2 mm — quite a
  generous catchment. It rescues near-misses, but it also means **two small
  controls closer than ~12 mm apart will fight over the same presses.** Keep
  interactive elements comfortably separated.
- **Extended hit areas.** Several controls have touch areas larger than their
  drawn size, because the drawn size was constrained by that 42 px bar. With the
  new panel's physical room, prefer making controls *actually* the right size
  over patching them with invisible margins.

**Unverified:** the old panel had physically rounded corners that swallowed
15–25 px, and the current layout carries 22–26 px side insets to keep controls
clear of them. The new panel appears rectangular, which would free those insets
— but this has not been confirmed on hardware. **Keep a safe margin until
someone looks at the real thing**, then reclaim it.

---

## 8. Open questions for you

1. **Calm or dense?** (§1) — the single decision that shapes everything else.
2. **Does the portrait 2:3 shape still suit Monitor?** The old panel was 1:1.22;
   this one is 1:1.5, noticeably taller and narrower. The hero readouts and the
   telemetry row may want a different arrangement.
3. **What should the extra 32 px of height do?** Taller heroes, an extra
   telemetry line, or simply more breathing room between blocks?
4. **Does the chrome still need 42 px?** At 6.5 mm it is generous; ~34 px would
   free 8 px of body height and still be a 5.2 mm target.
5. **Do you want real icons?** (§6) — newly possible, and it would change the
   character of the UI more than any other single choice.

## 9. Handing work back

Most useful, in order:

1. **Annotated screen mockups at exactly 320 × 480**, with pixel dimensions and
   the palette marked. Full-size, not scaled — this is a small pixel budget and
   scaled comps hide overflow.
2. **A spacing and type scale** in pixels: paddings, gaps, corner radii, font
   sizes per role, drawn from the sizes listed in §1.
3. **A colour ramp** with the dark end explicitly re-spaced for IPS (§3.2).
4. **A note on anything you are unsure is buildable** — cheaper to answer than to
   discover late.

Hex colours and pixel values are ideal. Figma/PNG/PDF are all fine. What is not
useful is anything depending on effects LVGL cannot render (§6) — a beautiful
comp built on blurs and vector art cannot ship.

Any question about what the device does, what a screen is for, or whether
something is buildable: ask. Getting that wrong on paper is free; getting it
wrong in firmware is not.
