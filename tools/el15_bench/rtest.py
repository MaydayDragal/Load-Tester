"""Run a full circuit-resistance test on a real EL15 from the PC.

    python rtest.py [fuse_A] [sweep_s] [out.csv]

A faithful port of the app's ResistanceTest engine — the same triangular ramp,
the same off-target gate, the same one-packet-delay repair, the same binning and
reliability rule — so the numbers it prints are the numbers the app would print
for the same sweep. Useful for checking the engine against hardware without a
phone in the loop, and for showing what the gate and the repair actually did.

SAFETY: as sweep.py. Every exit path leaves the load OFF.
"""
import asyncio
import csv
import sys
import time

from el15 import DEFAULT_ADDR, El15, MODE_CC, find

MIN_TEST_CURRENT = 0.05
SETPOINT_MS = 0.100
POLL_MS = 0.050
ABORT_BELOW_V = 8.0
MAX_CURRENT_A, MAX_POWER_W, MAX_VOLTAGE_V = 12.0, 150.0, 60.0
SAFETY = 0.80

OFF_TARGET_FLOOR_A, OFF_TARGET_FRACTION, OFF_TARGET_STEPS = 0.25, 0.20, 4.0
REPAIR_TOL_A, REPAIR_UNIQUE = 0.03, 2.5
NBINS = 32


def off_target_limit(target, ramp_step):
    return max(OFF_TARGET_FLOOR_A, OFF_TARGET_FRACTION * target, OFF_TARGET_STEPS * ramp_step)


def repair_off_target(reported, predicted):
    if reported <= 0 or predicted <= 0:
        return None
    cands = [2 * reported, (reported + 1) / 2, (reported + 3) / 4]
    best, best_v, runner = float("inf"), 0.0, float("inf")
    for c in cands:
        d = abs(c - predicted)
        if d < best:
            runner, best, best_v = best, d, c
        elif d < runner:
            runner = d
    if best > REPAIR_TOL_A or runner < REPAIR_UNIQUE * max(best, 1e-6):
        return None
    return best_v


class Fit:
    """The app's running least squares, six sums and nothing else."""

    def __init__(self):
        self.n = 0
        self.si = self.sv = self.sii = self.siv = self.svv = 0.0
        self.min_i = self.max_i = self.min_v = self.max_v = None

    def add(self, i, v):
        self.n += 1
        self.si += i; self.sv += v
        self.sii += i * i; self.siv += i * v; self.svv += v * v
        self.min_i = i if self.min_i is None else min(self.min_i, i)
        self.max_i = i if self.max_i is None else max(self.max_i, i)
        self.min_v = v if self.min_v is None else min(self.min_v, v)
        self.max_v = v if self.max_v is None else max(self.max_v, v)

    def result(self):
        if self.n < 8:
            return None
        n = self.n
        Sii = self.sii - self.si * self.si / n
        Siv = self.siv - self.si * self.sv / n
        Svv = self.svv - self.sv * self.sv / n
        if Sii < 1e-9 or (self.max_i - self.min_i) < 0.05:
            return None
        slope = Siv / Sii
        intercept = self.sv / n - slope * (self.si / n)
        r2 = (Siv * Siv) / (Sii * Svv) if Svv > 1e-9 else 0.0
        sse = max(Svv - slope * Siv, 0.0)
        se = (sse / ((n - 2) * Sii)) ** 0.5 if n > 2 else 0.0
        return max(-slope, 0.0), intercept, r2, se


async def main():
    fuse = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
    sweep_s = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    out = sys.argv[3] if len(sys.argv) > 3 else "rtest.csv"

    rows = []
    async with El15(DEFAULT_ADDR) as load:
        await load.set_mode(MODE_CC)
        await load.set_point(0.0)
        await load.set_load(False)
        await load.pump(1.2)
        st = load.last
        if st is None:
            print("no telemetry"); return
        voc = st.voltage
        if st.warning:
            print("device is in protection (%s)" % st.warning); return
        if not (0.1 < voc < MAX_VOLTAGE_V):
            print("source at %.3f V is outside the load's range" % voc); return

        peak = min(fuse * SAFETY, MAX_CURRENT_A, MAX_POWER_W / voc)
        start = MIN_TEST_CURRENT
        limiter = ("%.0f%% of the %.1f A fuse" % (SAFETY * 100, fuse)
                   if peak == fuse * SAFETY else
                   "the %.0f W limit at %.2f V" % (MAX_POWER_W, voc))
        print("Voc %.4f V   peak %.3f A (%s)   sweep %.0f s   temp %.1f C"
              % (voc, peak, limiter, sweep_s, st.temperature))

        half = sweep_s / 2
        ramp_step = (peak - start) * (SETPOINT_MS / half)
        t0 = time.perf_counter()
        target = start
        await load.set_point(target)
        await load.set_load(True)
        seen = 0
        next_set = time.perf_counter() + SETPOINT_MS
        last_on_push = time.perf_counter()
        while True:
            t = time.perf_counter() - t0
            if t >= sweep_s:
                break
            if time.perf_counter() >= next_set:
                next_set += SETPOINT_MS
                f = t / half if t < half else 2 - t / half
                target = max(start + (peak - start) * max(0.0, min(1.0, f)), MIN_TEST_CURRENT)
                await load.set_point(target)
            await load.poll()
            while seen < len(load.packets):
                p = load.packets[seen]; seen += 1
                if p.valid:
                    rows.append({"t": p.t, "tgt": target, "v": p.voltage, "i": p.current,
                                 "temp": p.temperature, "fan": p.fan, "on": p.load_on,
                                 "warn": p.warning, "raw": p.raw})
                    # The initial LOAD_ON does not always land, and the device
                    # answers a setpoint of 0.000 A by simply reporting the load
                    # off. Both are why the app re-asserts rather than assuming;
                    # without this an entire sweep can run at zero current. Rate
                    # limited so it cannot become a command flood of its own.
                    if not p.load_on and time.perf_counter() - last_on_push > 0.400:
                        last_on_push = time.perf_counter()
                        await load.set_point(target)
                        await load.set_load(True)
            await asyncio.sleep(POLL_MS)
        await load.set_load(False)
        await load.set_point(0.0)

    # ---- the engine, over what arrived ------------------------------------
    fit, fit_raw = Fit(), Fit()
    bins = [[0.0, 0.0, 0.0, 0, 0] for _ in range(NBINS)]
    pending = None
    last_i = last_t = None
    off_target = repaired = dropouts = 0
    peak_w = 0.0
    tmin = tmax = None
    events = []

    def accept(i, v, temp, fan, t):
        nonlocal last_i, last_t, peak_w, tmin, tmax
        fit.add(i, v)
        peak_w = max(peak_w, i * v)
        tmin = temp if tmin is None else min(tmin, temp)
        tmax = temp if tmax is None else max(tmax, temp)
        span = peak - start
        if span > 1e-6:
            k = int(((i - start) / span) * NBINS)
            k = max(0, min(NBINS - 1, k))
            b = bins[k]
            b[0] += i; b[1] += v; b[2] += temp; b[3] += 1; b[4] = max(b[4], fan)
        last_i, last_t = i, t

    for r in rows:
        if r["i"] > 0.0:
            fit_raw.add(r["i"], r["v"])
        if not r["on"]:
            dropouts += 1
            continue
        if abs(r["i"] - r["tgt"]) > off_target_limit(r["tgt"], ramp_step):
            if pending is not None:
                off_target += 1
            pending = r
            continue
        if pending is not None:
            p, pending = pending, None
            fixed = None
            if last_i is not None:
                dt = r["t"] - last_t
                pred = last_i if dt <= 0 else last_i + (r["i"] - last_i) * ((p["t"] - last_t) / dt)
                fixed = repair_off_target(p["i"], pred)
                events.append((p["t"], p["tgt"], p["i"], pred, fixed))
            if fixed is None:
                off_target += 1
            else:
                repaired += 1
                accept(fixed, p["v"], p["temp"], p["fan"], p["t"])
        accept(r["i"], r["v"], r["temp"], r["fan"], r["t"])
    if pending is not None:
        off_target += 1

    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_s", "target_a", "voltage_v", "current_a", "temp_c", "fan", "load_on", "raw"])
        for r in rows:
            w.writerow(["%.4f" % r["t"], r["tgt"], "%.4f" % r["v"], "%.4f" % r["i"],
                        r["temp"], r["fan"], int(r["on"]), r["raw"]])

    res = fit.result()
    if res is None:
        print("not enough usable samples for a fit"); return
    R, Voc, r2, se = res
    rel = (fit.n >= 20 and (fit.max_i - fit.min_i) > 0.05
           and (se <= 0.005 or (se / R <= 0.05 if R > 1e-4 else False)))

    print("\n" + "=" * 66)
    print("  CIRCUIT RESISTANCE        %.2f mOhm  +/- %.2f mOhm (1 sigma)"
          % (R * 1000, se * 1000))
    print("=" * 66)
    print("  open-circuit voltage      %.4f V" % Voc)
    print("  R-squared                 %.5f" % r2)
    print("  reliable                  %s" % ("yes" if rel else "no"))
    print("  est. short-circuit        %.1f A" % (Voc / R if R > 1e-6 else 0))
    print("  samples fitted            %d in %d bands"
          % (fit.n, sum(1 for b in bins if b[3])))
    print("  current measured          %.4f -> %.4f A" % (fit.min_i, fit.max_i))
    print("  voltage sag               %.4f V" % (fit.max_v - fit.min_v))
    print("  peak power                %.2f W" % peak_w)
    print("  temperature               %.1f -> %.1f C" % (tmin, tmax))
    print("  load dropouts             %d" % dropouts)
    print("  off-target readings       %d (not fitted)" % off_target)
    print("  recovered readings        %d (corruption inverted)" % repaired)
    print("  packets                   %d over %.1f s (%.1f/s)"
          % (len(rows), sweep_s, len(rows) / sweep_s))

    if events:
        print("\n  corrupted readings this sweep:")
        for t, tgt, rep, pred, fixed in events:
            print("    t=%6.2f s  commanded %.3f A  load reported %.4f A  ->  %s"
                  % (t, tgt, rep,
                     "recovered %.4f A" % fixed if fixed else "dropped (not identifiable)"))

    raw = fit_raw.result()
    if raw:
        print("\n  had every packet been fitted, as before the gate existed:")
        print("    R %.2f mOhm +/- %.2f, R2 %.5f, n=%d"
              % (raw[0] * 1000, raw[3] * 1000, raw[2], fit_raw.n))

    print("\n  V-I curve (band averages, both ramp directions)")
    print("    %8s %9s %8s %6s" % ("I (A)", "V (V)", "P (W)", "n"))
    for b in bins:
        if b[3]:
            i, v = b[0] / b[3], b[1] / b[3]
            print("    %8.4f %9.4f %8.2f %6d" % (i, v, i * v, b[3]))
    print("\n  raw packets written to %s" % out)


if __name__ == "__main__":
    if DEFAULT_ADDR is None:
        DEFAULT_ADDR = asyncio.run(find())
    asyncio.run(main())
