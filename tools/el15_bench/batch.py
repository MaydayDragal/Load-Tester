"""Run a series of R-tests back to back and report the repeatability.

    python batch.py [repeats] [fuse_A] [sweep_s] [prefix]

One connection, one sweep after another with a rest between, then the number
that matters for an instrument: how far apart the runs land, against how far
apart each run SAID it was. A single R-test cannot tell you that.

Guards, because this draws real current unattended: the series stops if the
load gets hot, if the source sags toward the bottom of its range, or if any
sweep trips protection. Every exit leaves the load off.
"""
import asyncio
import csv
import sys
import time

from el15 import DEFAULT_ADDR, El15, MODE_CC, find
from rtest import (Fit, MAX_CURRENT_A, MAX_POWER_W, MAX_VOLTAGE_V, MIN_TEST_CURRENT,
                   NBINS, POLL_MS, SAFETY, SETPOINT_MS, off_target_limit, repair_off_target)

REST_S = 8.0            # load off between sweeps
STOP_ABOVE_C = 60.0     # the load is rated well past this; stop long before it
STOP_BELOW_V = 11.0     # leave a 12 V source with something in it


async def one_sweep(load, fuse, sweep_s):
    """Prime, ramp up and back down, return every packet."""
    await load.set_mode(MODE_CC)
    await load.set_point(0.0)
    await load.set_load(False)
    await load.pump(1.2)
    st = load.last
    if st is None or st.warning or not (0.1 < st.voltage < MAX_VOLTAGE_V):
        return None, None
    voc = st.voltage
    peak = min(fuse * SAFETY, MAX_CURRENT_A, MAX_POWER_W / voc)
    start = MIN_TEST_CURRENT
    half = sweep_s / 2
    rows = []
    t0 = time.perf_counter()
    target = start
    await load.set_point(target)
    await load.set_load(True)
    seen = len(load.packets)
    next_set = time.perf_counter() + SETPOINT_MS
    last_on = time.perf_counter()
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
            if not p.valid:
                continue
            rows.append({"t": p.t, "tgt": target, "v": p.voltage, "i": p.current,
                         "temp": p.temperature, "fan": p.fan, "on": p.load_on,
                         "warn": p.warning, "raw": p.raw})
            if not p.load_on and time.perf_counter() - last_on > 0.400:
                last_on = time.perf_counter()
                await load.set_point(target)
                await load.set_load(True)
        await asyncio.sleep(POLL_MS)
    await load.set_load(False)
    await load.set_point(0.0)
    return rows, {"voc": voc, "peak": peak, "start": start,
                  "ramp_step": (peak - start) * (SETPOINT_MS / half)}


def analyse(rows, meta):
    """The app's engine over one sweep's packets."""
    fit, raw = Fit(), Fit()
    bins = [[0.0, 0.0, 0, 0] for _ in range(NBINS)]
    pending = None
    last_i = last_t = None
    off_target = repaired = dropouts = 0
    peak_w = 0.0
    tmin = tmax = None
    events = []
    start, peak = meta["start"], meta["peak"]

    def accept(i, v, temp, t):
        nonlocal last_i, last_t, peak_w, tmin, tmax
        fit.add(i, v)
        peak_w = max(peak_w, i * v)
        tmin = temp if tmin is None else min(tmin, temp)
        tmax = temp if tmax is None else max(tmax, temp)
        span = peak - start
        if span > 1e-6:
            k = max(0, min(NBINS - 1, int(((i - start) / span) * NBINS)))
            b = bins[k]; b[0] += i; b[1] += v; b[2] += 1
        last_i, last_t = i, t

    for r in rows:
        if r["i"] > 0.0:
            raw.add(r["i"], r["v"])
        if not r["on"]:
            dropouts += 1
            continue
        if abs(r["i"] - r["tgt"]) > off_target_limit(r["tgt"], meta["ramp_step"]):
            if pending is not None:
                off_target += 1
            pending = r
            continue
        if pending is not None:
            p, pending = pending, None
            fixed = None
            pred = None
            if last_i is not None:
                dt = r["t"] - last_t
                pred = last_i if dt <= 0 else last_i + (r["i"] - last_i) * ((p["t"] - last_t) / dt)
                fixed = repair_off_target(p["i"], pred)
            events.append((p["t"], p["tgt"], p["i"], pred, fixed))
            if fixed is None:
                off_target += 1
            else:
                repaired += 1
                accept(fixed, p["v"], p["temp"], p["t"])
        accept(r["i"], r["v"], r["temp"], r["t"])
    if pending is not None:
        off_target += 1

    res = fit.result()
    if res is None:
        return None
    R, Voc, r2, se = res
    return {"R": R, "Voc": Voc, "r2": r2, "se": se, "n": fit.n,
            "imin": fit.min_i, "imax": fit.max_i, "sag": fit.max_v - fit.min_v,
            "peak_w": peak_w, "tmin": tmin, "tmax": tmax, "dropouts": dropouts,
            "off_target": off_target, "repaired": repaired, "events": events,
            "raw": raw.result(), "packets": len(rows),
            "reliable": fit.n >= 20 and (fit.max_i - fit.min_i) > 0.05
                        and (se <= 0.005 or (R > 1e-4 and se / R <= 0.05))}


async def main():
    repeats = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    fuse = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
    sweep_s = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
    prefix = sys.argv[4] if len(sys.argv) > 4 else "batch"

    results = []
    stopped = ""
    async with El15(DEFAULT_ADDR) as load:
        for k in range(1, repeats + 1):
            rows, meta = await one_sweep(load, fuse, sweep_s)
            if rows is None:
                stopped = "no telemetry, or the device is in protection"
                break
            res = analyse(rows, meta)
            with open("%s_%02d.csv" % (prefix, k), "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["t_s", "target_a", "voltage_v", "current_a", "temp_c",
                            "fan", "load_on", "raw"])
                for r in rows:
                    w.writerow(["%.4f" % r["t"], r["tgt"], "%.4f" % r["v"], "%.4f" % r["i"],
                                r["temp"], r["fan"], int(r["on"]), r["raw"]])
            if res is None:
                print("%2d  no fit" % k)
                continue
            results.append(res)
            print("%2d  R %6.2f mOhm +/- %4.2f   Voc %7.4f   R2 %.4f   n=%3d   "
                  "%2d drop  %d off  %d fixed   %.1f-%.1f C"
                  % (k, res["R"] * 1000, res["se"] * 1000, res["Voc"], res["r2"], res["n"],
                     res["dropouts"], res["off_target"], res["repaired"],
                     res["tmin"], res["tmax"]))
            for t, tgt, rep, pred, fixed in res["events"]:
                print("      corrupted: commanded %.3f A, reported %.4f A, ramp %.4f A -> %s"
                      % (tgt, rep, pred if pred else 0,
                         "recovered %.4f A" % fixed if fixed else "dropped"))
            if res["tmax"] >= STOP_ABOVE_C:
                stopped = "load reached %.1f C" % res["tmax"]; break
            if res["Voc"] <= STOP_BELOW_V:
                stopped = "source down to %.3f V" % res["Voc"]; break
            if k < repeats:
                await load.pump(REST_S)

    if not results:
        print("no results%s" % (" (%s)" % stopped if stopped else ""))
        return
    Rs = [r["R"] * 1000 for r in results]
    n = len(Rs)
    mean = sum(Rs) / n
    sd = (sum((x - mean) ** 2 for x in Rs) / (n - 1)) ** 0.5 if n > 1 else 0.0
    reported = sum(r["se"] * 1000 for r in results) / n
    print("\n" + "=" * 70)
    print("  %d sweeps" % n)
    print("  resistance          mean %.2f mOhm   spread %.2f   sd %.2f"
          % (mean, max(Rs) - min(Rs), sd))
    print("  range               %.2f .. %.2f mOhm" % (min(Rs), max(Rs)))
    print("  reported sigma      %.2f mOhm (mean of the runs)" % reported)
    print("  honesty             true scatter is %.2fx the reported sigma"
          % (sd / reported if reported else 0))
    print("  Voc drift           %.4f -> %.4f V (%.0f mV)"
          % (results[0]["Voc"], results[-1]["Voc"],
             (results[0]["Voc"] - results[-1]["Voc"]) * 1000))
    print("  temperature         %.1f -> %.1f C"
          % (results[0]["tmin"], results[-1]["tmax"]))
    print("  corrupted readings  %d over %d crossings (%.0f%%), %d recovered, %d dropped"
          % (sum(len(r["events"]) for r in results), 2 * n,
             100.0 * sum(len(r["events"]) for r in results) / (2 * n),
             sum(r["repaired"] for r in results), sum(r["off_target"] for r in results)))
    print("  all runs reliable   %s" % ("yes" if all(r["reliable"] for r in results) else "no"))
    if stopped:
        print("  STOPPED EARLY       %s" % stopped)


if __name__ == "__main__":
    if DEFAULT_ADDR is None:
        DEFAULT_ADDR = asyncio.run(find())
    asyncio.run(main())
