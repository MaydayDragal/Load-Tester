"""Drive a current profile on a real EL15 and log every packet, raw bytes included.

    python sweep.py <profile> [out.csv] [address]

Profiles (see PROFILES): triangle, fine, stairs, cycle, hold, scan, lowcycle.
With no address it uses EL15_ADDR / the built-in default, then falls back to a
scan.

SAFETY. Every exit path — normal end, exception, Ctrl-C, abort — goes through
El15's context manager, which writes LOAD OFF + setpoint 0 three times before
disconnecting. The run also aborts on a protection trip or a collapsing source,
and no profile may command more than HARD_MAX_A.

This draws REAL CURRENT through whatever is wired to the load. Know what is
connected before running it.
"""
import asyncio
import csv
import sys
import time

from el15 import DEFAULT_ADDR, El15, MODE_CC, find

SETPOINT_MS = 0.100      # how often the commanded current is rewritten
POLL_MS = 0.050
MIN_TEST_CURRENT = 0.05
ABORT_BELOW_V = 9.0
HARD_MAX_A = 4.0


def triangle(t, total=15.0, lo=0.05, hi=4.0):
    """The app's R-test sweep, for reproducing a reported result."""
    if t >= total:
        return None
    half = total / 2
    f = t / half if t < half else 2 - t / half
    return lo + (hi - lo) * max(0.0, min(1.0, f))


def fine(t, total=80.0, lo=0.80, hi=1.60):
    """~20 mA/s across the suspect window, to place a threshold to a few mA."""
    if t >= total:
        return None
    half = total / 2
    f = t / half if t < half else 2 - t / half
    return lo + (hi - lo) * max(0.0, min(1.0, f))


def stairs(t, dwell=3.0):
    """Hold each level: separates a fault that needs a CROSSING from one that
    happens while the current sits still."""
    levels = [1.00, 1.05, 1.10, 1.15, 1.20, 1.25, 1.30, 1.35,
              1.30, 1.25, 1.20, 1.15, 1.10, 1.05, 1.00]
    k = int(t / dwell)
    return None if k >= len(levels) else levels[k]


def cycle(t, total=48.0, lo=1.10, hi=1.30, period=4.0):
    """Cross the threshold 24 times, so the per-crossing rate is measurable."""
    if t >= total:
        return None
    f = (t % period) / period
    return lo + (hi - lo) * (2 * f if f < 0.5 else 2 * (1 - f))


def hold(t, total=45.0):
    """Sit either side of the threshold, then on it. A crossing-triggered fault
    produces nothing here; a hunting comparator produces events at 1.20 A."""
    if t >= total:
        return None
    return 1.15 if t < 15 else (1.25 if t < 30 else 1.20)


def scan(t, total=140.0, lo=0.05, hi=5.0):
    """Slow full-range triangle: finds every current the fault fires at rather
    than only the one already suspected."""
    if t >= total:
        return None
    half = total / 2
    f = t / half if t < half else 2 - t / half
    return lo + (hi - lo) * max(0.0, min(1.0, f))


def lowcycle(t, total=40.0, lo=0.08, hi=0.20, period=4.0):
    """Cross a possible low-current boundary repeatedly."""
    if t >= total:
        return None
    f = (t % period) / period
    return lo + (hi - lo) * (2 * f if f < 0.5 else 2 * (1 - f))


PROFILES = {"triangle": triangle, "fine": fine, "stairs": stairs, "cycle": cycle,
            "hold": hold, "scan": scan, "lowcycle": lowcycle}


async def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "triangle"
    out = sys.argv[2] if len(sys.argv) > 2 else "%s.csv" % which
    addr = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_ADDR
    if which not in PROFILES:
        print("profiles: %s" % ", ".join(sorted(PROFILES)))
        return
    profile = PROFILES[which]

    rows, target, aborted = [], 0.0, ""

    async with El15(addr) as load:
        # ---- prime: load off, setpoint 0, read the open-circuit voltage ----
        await load.set_mode(MODE_CC)
        await load.set_point(0.0)
        await load.set_load(False)
        await load.pump(1.2)
        st = load.last
        if st is None:
            print("no telemetry"); return
        print("Voc %.4f V, temp %.1f C, load %s" % (st.voltage, st.temperature, st.load_on))
        if st.warning:
            print("device is in protection (%s) - not starting" % st.warning); return

        # ---- run the profile ----
        t0 = time.perf_counter()
        target = max(profile(0.0), MIN_TEST_CURRENT)
        await load.set_point(min(target, HARD_MAX_A))
        await load.set_load(True)
        seen = 0
        next_set = time.perf_counter() + SETPOINT_MS

        while True:
            want = profile(time.perf_counter() - t0)
            if want is None:
                break
            if time.perf_counter() >= next_set:
                next_set += SETPOINT_MS
                target = min(max(want, MIN_TEST_CURRENT), HARD_MAX_A)
                await load.set_point(target)
            await load.poll()

            # Drain what landed, tagging each packet with the live command.
            while seen < len(load.packets):
                p = load.packets[seen]
                seen += 1
                if not p.valid:
                    rows.append((p.t, target, "", "", "", "", "", 0, p.raw, "BADCRC"))
                    continue
                rows.append((p.t, target, p.voltage, p.current, p.setpoint,
                             p.temperature, p.runtime, p.fan, p.raw,
                             "LOADOFF" if not p.load_on else p.warning))
                if p.warning:
                    aborted = "protection: %s" % p.warning
                if p.voltage < ABORT_BELOW_V and p.load_on:
                    aborted = "source collapsed to %.2f V" % p.voltage
            if aborted:
                break
            await asyncio.sleep(POLL_MS)

        await load.set_load(False)
        await load.set_point(0.0)
        elapsed = time.perf_counter() - t0

    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_s", "target_a", "voltage_v", "current_a", "setpoint_echo_a",
                    "temp_c", "runtime_s", "fan", "raw", "flags"])
        for r in rows:
            w.writerow(["%.4f" % r[0]] + list(r[1:]))

    good = [r for r in rows if r[2] != ""]
    print("%s: %.1f s, %d packets (%d valid) -> %s" % (which, elapsed, len(rows), len(good), out))
    if good:
        cur = [float(r[3]) for r in good]
        print("current measured %.4f .. %.4f A" % (min(cur), max(cur)))
    if aborted:
        print("ABORTED: %s" % aborted)


if __name__ == "__main__":
    if DEFAULT_ADDR is None:
        DEFAULT_ADDR = asyncio.run(find())
    asyncio.run(main())
