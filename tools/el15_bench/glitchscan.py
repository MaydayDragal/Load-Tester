"""Find corrupted samples in captured sweeps and identify the corruption.

    python glitchscan.py capture1.csv capture2.csv ...

A corrupted sample is a SPIKE: it departs from the line between its neighbours
AND comes straight back, so the two neighbours agree with each other. That
second condition is what makes this trustworthy — a plain "differs from the
local median" test calls ordinary quantisation on a steeply falling ramp
corrupt, because there the neighbours disagree with each other too.

For each spike it reports which float-field corruption maps the true value
(estimated from the neighbours) onto the reported one. The load's fault shifts
the significand relative to the exponent, which is why the errors are these
particular shapes and not a constant offset or a constant ratio.
"""
import csv
import sys

NEIGHBOURS_AGREE_A = 0.06     # |before - after| must be under this
SPIKE_A = 0.06                # and the sample must be at least this far off
SPIKE_FRACTION = 0.06


def load(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r["voltage_v"] == "" or r["flags"] == "LOADOFF":
                continue
            rows.append({"t": float(r["t_s"]), "tgt": float(r["target_a"]),
                         "i": float(r["current_a"]), "v": float(r["voltage_v"]),
                         "raw": r["raw"]})
    return rows


def signature(bad, true):
    """Which one-bit-shift corruption turns `true` into `bad`?

    For a value v in [1,2) the float's exponent is fixed, so shifting the
    significand left one place gives 1+2f = 2v-1, two places 4v-3; dropping the
    exponent's LSB instead leaves the significand alone and halves the value.
    """
    for name, implied in (("exponent -1   (v/2)", bad * 2),
                          ("mantissa <<1  (2v-1)", (bad + 1) / 2),
                          ("mantissa <<2  (4v-3)", (bad + 3) / 4)):
        if abs(implied - true) < 0.035:
            return "%-20s implies true %.4f" % (name, implied)
    return "unclassified"


def scan(path):
    rows = load(path)
    out = []
    for k in range(1, len(rows) - 1):
        a, b = rows[k - 1]["i"], rows[k + 1]["i"]
        i = rows[k]["i"]
        if min(a, b, i) < 0.02 or abs(a - b) > NEIGHBOURS_AGREE_A:
            continue
        mid = (a + b) / 2
        if abs(i - mid) < max(SPIKE_A, SPIKE_FRACTION * mid):
            continue
        out.append((rows[k], mid))
    return rows, out


def main(paths):
    total = 0
    everything = []
    for path in paths:
        rows, spikes = scan(path)
        total += len(rows)
        print("\n%s: %d conducting packets, %d spikes" % (path, len(rows), len(spikes)))
        for r, mid in spikes:
            everything.append(mid)
            print("  t=%7.3f cmd %6.3f  reported %8.4f  true ~%.4f  err %+.3f   %s"
                  % (r["t"], r["tgt"], r["i"], mid, r["i"] - mid, signature(r["i"], mid)))
            print("      %s" % r["raw"])

        # The voltage field in the same frames, by the same test.
        vbad = 0
        for k in range(1, len(rows) - 1):
            a, b, v = rows[k - 1]["v"], rows[k + 1]["v"], rows[k]["v"]
            if abs(a - b) > 0.05:
                continue
            if abs(v - (a + b) / 2) > 0.08:
                vbad += 1
        print("  voltage spikes by the same test: %d" % vbad)

    if everything:
        print("\n%d spikes over %d packets; true current at every one: %.4f .. %.4f A"
              % (len(everything), total, min(everything), max(everything)))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
    else:
        main(sys.argv[1:])
