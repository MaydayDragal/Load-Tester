#!/usr/bin/env python3
"""Repair an EL15 report CSV whose SD card lost part of the file.

Usage:  python repair_report_csv.py D:\\BATT_007.CSV [-o out.csv]

Why this exists
---------------
On 2026-08-03 the first real-current capacity run (BATT_007.CSV, 317 627 B)
came back with two regions of pure-random bytes in the datapoint block:

    114688 .. 131071   16384 B   (exactly 16 KB, 16 KB-aligned)
    311296 .. EOF       6331 B   (16 KB-aligned, runs to end of file)

Both are aligned to, and sized in, the card's 16 KB allocation unit; the
contents are uniformly random (7.99 bits/byte, no repeated 16-byte blocks, no
recoverable text). The firmware's byte accounting across the gaps is exact --
the missing bytes are precisely the rows that belong at those offsets -- and
`USE_SD_CRC=1` means a corrupted SPI transfer would have been rejected by the
card and surfaced as a write error, which would have deleted the file rather
than saving it. So every byte left the controller intact and was acknowledged;
the CARD accepted ~22 KB of writes, reported success, and did not retain them.

That is a card fault, not a firmware one, and it is not repairable -- the
readings in those regions are gone. What this script does is make the file
honest and usable again: strip the garbage, keep every intact row, and mark the
holes explicitly so a gap is never mistaken for a measurement artefact.

The summary block at the top of the report is computed by the instrument from
its own accumulators, NOT from these rows, so capacity / energy / duration /
state-of-health are unaffected by the loss.

The original file is never modified.
"""

import argparse
import os
import re
import sys

# A byte is "report text" if it could have come out of report.h's fpf().
def is_text(b: int) -> bool:
    return 32 <= b < 127 or b in (10, 13)


def find_garbage(data: bytes, merge_gap: int = 200):
    """Byte ranges that cannot have been written by the firmware.

    Short runs are merged: random data contains incidental printable bytes, so a
    corrupt region reads as many small runs rather than one clean block.
    """
    runs, start = [], None
    for i, b in enumerate(data):
        if not is_text(b):
            if start is None:
                start = i
        elif start is not None:
            runs.append((start, i))
            start = None
    if start is not None:
        runs.append((start, len(data)))

    merged = []
    for s, e in runs:
        if merged and s - merged[-1][1] < merge_gap:
            merged[-1] = (merged[-1][0], e)
        else:
            merged.append([s, e])
    return [(s, e) for s, e in merged]


ROW = re.compile(r"^\d+(,-?[\d.]+){7}$")


def row_time(line: str):
    return int(line.split(",", 1)[0]) if ROW.match(line) else None


def times(text: str):
    ts = [row_time(l) for l in text.split("\n")]
    return [t for t in ts if t is not None]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="the damaged report, e.g. D:\\BATT_007.CSV")
    ap.add_argument("-o", "--out", help="output path (default: <name>_repaired.CSV)")
    args = ap.parse_args()

    if not os.path.isfile(args.csv):
        print(f"error: {args.csv} not found", file=sys.stderr)
        return 1
    raw = open(args.csv, "rb").read()
    out_path = args.out or re.sub(r"\.csv$", "", args.csv, flags=re.I) + "_repaired.CSV"

    bad = find_garbage(raw)
    print(f"{args.csv}: {len(raw)} bytes")
    if not bad:
        print("  no corruption found - nothing to repair.")
        return 0

    print(f"  {len(bad)} corrupt region(s):")
    for s, e in bad:
        print(f"    {s:>8} .. {e:<8} {e-s:>7} B   "
              f"(offset/16K = {s/16384:.3f}, length/16K = {(e-s)/16384:.3f})")

    # Split the file into the surviving segments between the corrupt regions,
    # trimming each back to whole rows: the cut always lands mid-row, and half a
    # row is worse than no row.
    segments, pos = [], 0
    for s, e in bad:
        segments.append((raw[pos:s].decode("latin1"), pos == 0))
        pos = e
    if pos < len(raw):
        segments.append((raw[pos:].decode("latin1"), False))

    cleaned = []
    for text, is_first in segments:
        if not is_first and "\n" in text:
            text = text[text.index("\n") + 1:]      # drop the fragment we resumed into
        if "\n" in text:
            text = text[:text.rindex("\n") + 1]     # drop the fragment we were cut off in
        if text.strip():
            cleaned.append(text)

    # Total duration comes from the summary block, so the trailing gap can state
    # how much of the run it swallowed.
    m = re.search(r"^duration,(\d+),s", raw.decode("latin1"), re.M)
    duration = int(m.group(1)) if m else None

    out = ""
    for i, text in enumerate(cleaned):
        out += text
        if i == len(cleaned) - 1:
            break
        after = times(cleaned[i + 1])
        before = times(text)
        if before and after:
            step = 2
            ta = times(text)
            if len(ta) >= 2:
                step = ta[-1] - ta[-2]
            lost = max((after[0] - before[-1]) // step - 1, 0)
            out += (f"# *** DATA GAP: t={before[-1]+step}..{after[0]-step} s missing "
                    f"({lost} rows).\n"
                    f"#     This block of the file was lost by the SD card; the readings "
                    f"either side are intact. ***\n")

    last = times(cleaned[-1]) if cleaned else []
    if bad and bad[-1][1] >= len(raw) and last and duration:
        ta = times(cleaned[-1])
        step = ta[-1] - ta[-2] if len(ta) >= 2 else 4
        out += (f"# *** DATA GAP: t={last[-1]+step}..{duration} s missing (end of run) - the "
                f"tail of\n"
                f"#     the file was lost by the SD card. The summary block above is computed "
                f"by the\n"
                f"#     instrument, NOT from these rows, so capacity / energy / duration are\n"
                f"#     unaffected. ***\n")

    with open(out_path, "w", newline="") as f:
        f.write(out)

    kept = len(times(out))
    m = re.search(r"# Datapoints \((\d+) samples", out)
    claimed = int(m.group(1)) if m else None
    stray = sum(1 for c in out if not is_text(ord(c)))
    print(f"\n  wrote {out_path}")
    print(f"    {len(out)} bytes, {kept} data rows"
          + (f" of the {claimed} logged ({kept/claimed*100:.1f}% recovered)" if claimed else ""))
    print(f"    non-text bytes remaining: {stray}")
    print(f"    original left untouched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
