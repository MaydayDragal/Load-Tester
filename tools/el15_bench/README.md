# EL15 bench harness

Drives a real EL15 over BLE **from a PC** and logs every status frame with its
raw bytes. Not part of the app or the firmware build — a debugging instrument
for questions about what the load itself does.

It exists because the phone app records decoded values, and some questions can
only be answered from the wire. The ~1.2 A current-reading fault
(`firmware/RTEST_ACCURACY.md` §7) was diagnosed here: the decoded numbers said
"a wrong current", the raw frames said *which bits* were wrong, which is what
turned a hypothesis into a mechanism.

## Use

```
pip install bleak
python sweep.py cycle out.csv        # drive a profile, capture every packet
python glitchscan.py out.csv         # find corrupted samples and name the corruption
```

The address defaults to the unit this was written against; override with
`EL15_ADDR=AA:BB:...` or pass it as the third argument. `el15.find()` scans if
you have neither.

**This draws real current.** Know what is wired to the load first. Every exit
path — normal end, exception, Ctrl-C, protection trip, a collapsing source —
goes through the context manager, which writes LOAD OFF + setpoint 0 three
times before disconnecting, and no profile may command past `HARD_MAX_A`.

## Profiles

| | what it answers |
|---|---|
| `triangle` | the app's own R-test sweep — reproduces a reported result |
| `fine` | ~20 mA/s across a suspect window: places a threshold to a few mA |
| `stairs` | dwell at fixed levels: does the fault need a CROSSING? |
| `cycle` | 24 crossings of one threshold: what is the per-crossing rate? |
| `hold` | sit still either side of, and on, a threshold — the negative control |
| `scan` | slow full-range: is there more than one threshold? |
| `lowcycle` | the same question at the low-current end |

`el15.py` is a faithful port of the app's protocol rules — sum-to-zero command
checksums, control writes paced 50 ms apart with polls held 20 ms behind them,
28-byte reassembly with header resync — so what it measures is what the app
would see, not an artefact of a different driver.

## captures/

The evidence behind §7, kept because the conclusion rests on specific frames:

- **`cycle-1.2A-crossings.csv`** — 24 crossings of 1.10↔1.30 A. Nine corrupted
  samples, every one with the true current in 1.198–1.207 A.
- **`rtest-triangle.csv`** — the app's 0.05→4→0.05 A sweep, run from the PC.
  Two events, at the same current, in both ramp directions: the blip is the
  load's, not the phone's.
- **`hold-steady-control.csv`** — 45 s held at 1.15, 1.25 and 1.20 A. Zero
  events. The fault needs the current to be moving through the threshold.
