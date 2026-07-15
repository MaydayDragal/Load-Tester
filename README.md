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
