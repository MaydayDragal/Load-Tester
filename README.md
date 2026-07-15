# EL15 Load Control — Android

An Android app to control an **EL15 electronic load** (a DC "load tester") over
Bluetooth Low Energy, from your phone.

It re-implements the BLE protocol used by the desktop
[DM40GUI](https://github.com/maj113/DM40GUI) project (see its
[Device Support](https://github.com/maj113/DM40GUI/blob/master/README.md#device-support)
section — the EL15 is the electronic load). No PC required.

## Features

- **Scan & connect** to the EL15 over BLE (filters on the FFF0 service).
- **Live readouts**, updated ~2×/sec: voltage, current, power, active mode,
  runtime, temperature, fan speed.
- **Mode switching**: CC, CV, CR, CP, CAP (capacity), DCR (internal resistance).
- **Setpoint control**: set the target current / voltage / resistance / power for
  the active mode (the input's label + units follow the mode automatically).
- **Load ON/OFF** toggle and **keypad lock**.
- **Protection banner** (REV / UVP / other) when the load trips.
- **CAP** mode shows accumulated energy (Wh) and capacity (Ah);
  **DCR** mode shows measured milliohms.
- **Circuit Resistance Test** (see below): a dynamic, fuse-rating-driven load
  sweep that measures a circuit's series resistance and produces a graph.

## Circuit Resistance Test

A custom test mode that measures the **series resistance of a circuit** (source
internal resistance + wiring + connections) by loading it, rather than the
device's built-in DCR (which measures a component's internal resistance).

**How it works** — the EL15 is put into constant-current (CC) mode and made to
sink a rising ladder of currents. As current increases, terminal voltage sags
according to the circuit's resistance. The relationship is a straight line:

```
V = Voc − R · I     ⇒     R = −dV/dI   (slope of the V–I line)
```

The app records (V, I) at each step and fits a least-squares line; the slope is
the circuit resistance and the intercept is the open-circuit voltage.

**Dynamic, fuse-driven** — you enter the circuit's **fuse rating**. The test
never draws more than **80% of the fuse rating**, and the step size scales with
the rating: a 2 A circuit is probed in tens of mA, a 30 A circuit in amps.
Before starting, the app shows a confirmation with the exact peak current it will
draw and what limits it. It aborts immediately if the load trips a protection
(REV/UVP/OVP), and always turns the load off when finished.

**Kept within the EL15's ratings** — the sweep is clamped so it can never command
more than the hardware allows (ALIENTEK EL15: **12 A**, **150 W**, **60 V**).
Because dissipation is `V × I`, **power is usually the binding limit**: on a 48 V
source, 150 W permits only ~3.1 A regardless of a larger fuse. The test measures
the open-circuit voltage while priming, then caps the ladder at:

```
I_max = min( 80% × fuse ,  12 A ,  150 W ÷ Voc )
```

Bounding by `150 W ÷ Voc` is conservative — terminal voltage only sags under
load, so real power stays under 150 W. Each commanded step is clamped again live,
and the test aborts if the source is outside 0.1–60 V. (Example: a 48 V source
with a 30 A fuse would naively pull 24 A / ~1150 W; the test instead peaks at
~3.1 A / ~148 W.)

**Configurable sweep** — the test card exposes three tunables:

| Option | Range | Default | Effect |
|--------|-------|---------|--------|
| **Steps** | 3–20 | 8 | number of current levels in the ladder |
| **Settle ms** | 100–5000 | 800 | wait after each setpoint change before sampling |
| **Sample ms** | 200–8000 | 1500 | averaging window per step |

More steps + a longer sample window give a tighter fit at the cost of runtime
(the card shows the estimated duration when you start).

**Results** — when the sweep completes you get:

- the computed **circuit resistance** (auto Ω / mΩ) and open-circuit voltage,
- a **Voltage-vs-Current graph** with the fitted resistance line,
- a **Voltage & Current per-step** trend graph,
- the raw data table (per-point resistance included), and a fit-quality (R²)
  confidence check, and
- **Print** (Android print dialog → paper or PDF) and **Share** buttons that
  render a clean white-background report.

Implemented in
[`CircuitResistanceTester.kt`](app/src/main/java/com/loadtester/el15/CircuitResistanceTester.kt)
(sweep engine + regression),
[`ResistanceChartView.kt`](app/src/main/java/com/loadtester/el15/ResistanceChartView.kt)
(dependency-free Canvas charts), and
[`ResultActivity.kt`](app/src/main/java/com/loadtester/el15/ResultActivity.kt)
(results, print/share).

> ⚠ **Safety** — this drives real current through your circuit. Only test
> sources and wiring rated for the peak current shown, keep the fuse rating
> honest, and stay clear of the setup while a test runs.

## Protocol

The app talks to the standard Nordic-style GATT service exposed by the EL15:

| Role   | UUID                                     |
|--------|------------------------------------------|
| Service| `0000fff0-0000-1000-8000-00805f9b34fb`   |
| Notify | `0000fff1-0000-1000-8000-00805f9b34fb`   |
| Write  | `0000fff3-0000-1000-8000-00805f9b34fb`   |

**Commands** (written to FFF3, no response):

| Action        | Frame (hex)                         |
|---------------|-------------------------------------|
| Poll status   | `AF 07 03 08 00 3F`                 |
| Load ON       | `AF 07 03 09 01 04`                 |
| Load OFF      | `AF 07 03 09 01 00`                 |
| Lock keypad   | `AF 07 03 09 01 01`                 |
| Set mode      | `AF 07 03 03 01 <mode>`             |
| Set setpoint  | `AF 07 03 04 04 <float32 LE>`       |

Mode IDs: CC `0x01`, CAP `0x02`, CV `0x09`, DCR `0x0A`, CR `0x11`, CP `0x19`.

**Status notification** (28 bytes, header `DF 07 03 08`, CRC = `sum(bytes) & 0xFF == 0`):

- voltage: float32 LE at offset 7
- current: float32 LE at offset 11
- power: voltage × current
- byte 5: mode (low 5 bits) + fan speed (bits 6–7); byte 6: load/lock bits +
  fan MSB + protection nibble
- mode-specific tail: CC/CV/CR/CP → runtime(i32)/temp(f32)/setpoint(f32);
  CAP → energy/capacity; DCR → I1/I2/resistance

The full port lives in
[`El15Protocol.kt`](app/src/main/java/com/loadtester/el15/El15Protocol.kt).

## Demo device (no hardware needed)

Tap **Scan & Connect** and pick **🧪 EL15 Demo (Simulator)** at the top of the
device list. This connects to a built-in fake load that models a virtual circuit
(≈12.6 V source behind ≈0.35 Ω) and behaves like a real EL15 — it honours
mode/setpoint/load commands and streams live readings, so every feature,
including the circuit-resistance test, works end-to-end. A resistance test
against the demo recovers ≈0.35 Ω / ≈12.6 V. Implemented in
[`El15Simulator.kt`](app/src/main/java/com/loadtester/el15/El15Simulator.kt);
the real transport and the demo share the
[`El15Controller`](app/src/main/java/com/loadtester/el15/El15Controller.kt)
interface so the rest of the app is unaware which one it is driving.

## Building

Requires JDK 17 and the Android SDK (API 34).

```bash
./gradlew assembleDebug
# -> app/build/outputs/apk/debug/app-debug.apk
```

A GitHub Actions workflow ([`.github/workflows/android.yml`](.github/workflows/android.yml))
builds the debug APK on every push and uploads it as a downloadable artifact
named **el15-load-control-debug**.

## Installing

1. Download `app-debug.apk` (from a CI run's artifacts, or your own build).
2. Copy it to your Android phone and open it; allow installation from unknown
   sources when prompted.
3. Launch **EL15 Load Control**, grant Bluetooth permissions, tap
   **Scan & Connect**, and pick your EL15.

- **minSdk**: Android 7.0 (API 24) · **targetSdk**: 34
- Permissions: `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT` (Android 12+) or
  `BLUETOOTH` + `ACCESS_FINE_LOCATION` (Android 11 and below).

## Safety

This app drives real power hardware. Double-check the setpoint before enabling the
load, respect your device's ratings, and never leave a running load unattended.

## Credits

Protocol reverse-engineering and reference implementation:
[maj113/DM40GUI](https://github.com/maj113/DM40GUI). This app is an independent
Android client for the same hardware.
