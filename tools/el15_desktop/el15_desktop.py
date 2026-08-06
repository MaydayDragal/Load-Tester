"""EL15 Load Control — desktop.

A PC front end for the ALIENTEK EL15 electronic load: live monitoring, manual
control, and the circuit-resistance sweep, over the same BLE protocol the phone
app speaks.

It is a FRONT END, not a second implementation. The protocol client and the
R-test engine are imported from `tools/el15_bench`, which is the code that was
validated against real hardware — command checksums, control-write pacing, frame
reassembly, the off-target gate, the spike test and the repair. A GUI that
reimplemented any of that would drift from the phone and the board, and the
whole point of this project is that the three agree.

SAFETY. The load draws real current through whatever is wired to it. Every path
out of a connected session — the Disconnect button, closing the window, a BLE
error, a failed test — goes through one teardown that writes LOAD OFF and
setpoint 0 several times before dropping the link. The emergency stop is always
enabled, even while a test runs, and never asks for confirmation.

Run:  python el15_desktop.py
"""
import asyncio
import csv
import queue
import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

# The client and engine live with the bench tools; see the note above.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "el15_bench"))

from el15 import (DEFAULT_ADDR, MAX_CURRENT_A, MAX_POWER_W, MAX_VOLTAGE_V,  # noqa: E402
                  MODE_CC, MODE_NAMES, El15, find)
from rtest import (MIN_TEST_CURRENT, NBINS, SAFETY, Fit,  # noqa: E402
                   off_target_limit, repair_off_target)

POLL_S = 0.050
SETPOINT_S = 0.100
REPAIR_TOL_A = 0.03
SPIKE_A = 0.05

# Modes the user can pick, with the unit and precision of their setpoint.
SETTABLE = [
    (MODE_CC, "CC", "A", 3), (0x09, "CV", "V", 3), (0x11, "CR", "Ω", 1),
    (0x19, "CP", "W", 2), (0x02, "CAP", "A", 3), (0x0A, "DCR", "A", 3),
]

BG = "#12161c"
PANEL = "#1b212a"
INK = "#e6edf3"
MUTED = "#8b98a5"
GREEN = "#4caf50"
AMBER = "#ffb300"
RED = "#ef5350"
BLUE = "#7ba1c9"


# ---------------------------------------------------------------------------
# Worker: one asyncio loop on its own thread. Tk never touches BLE, BLE never
# touches Tk; everything crosses on a queue that the UI drains on a timer.
# ---------------------------------------------------------------------------
class Worker:
    def __init__(self, emit):
        self.emit = emit                  # called from the BLE thread
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.load = None
        self.stop_session = None
        self.rtest_task = None
        self.poll_s = POLL_S

    def _run(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def start(self):
        self.thread.start()

    def submit(self, coro):
        """Fire a coroutine on the BLE loop; surface failures rather than
        letting them vanish into a task nobody awaits."""
        fut = asyncio.run_coroutine_threadsafe(coro, self.loop)

        def done(f):
            try:
                f.result()
            except Exception as e:                      # noqa: BLE001
                self.emit("error", "%s: %s" % (type(e).__name__, e))
        fut.add_done_callback(done)
        return fut

    # ---- session ----------------------------------------------------------
    async def _session(self, address):
        stop = asyncio.Event()
        self.stop_session = stop
        try:
            # The context manager is the safety guarantee: LOAD OFF + setpoint 0
            # three times on the way out, whatever ends the session.
            async with El15(address, on_status=self._on_status, verbose=False) as load:
                self.load = load
                self.emit("connected", address)
                while not stop.is_set():
                    await load.poll()
                    await asyncio.sleep(self.poll_s)
        finally:
            self.load = None
            self.stop_session = None
            self.emit("disconnected", None)

    def _on_status(self, st):
        self.emit("status", st)

    def connect(self, address):
        self.submit(self._session(address))

    def disconnect(self):
        if self.stop_session is not None:
            self.loop.call_soon_threadsafe(self.stop_session.set)

    async def _scan(self):
        self.emit("scanning", None)
        addr = await find(timeout=8.0)
        self.emit("found", addr)

    def scan(self):
        self.submit(self._scan())

    # ---- commands ---------------------------------------------------------
    async def _cmd(self, fn, *a):
        if self.load is None:
            return
        await fn(*a)

    def set_mode(self, mode):
        self.submit(self._cmd(lambda: self.load.set_mode(mode)))

    def set_point(self, value):
        self.submit(self._cmd(lambda: self.load.set_point(value)))

    def set_load(self, on):
        self.submit(self._cmd(lambda: self.load.set_load(on)))

    async def _estop(self):
        """Kill the load now. Stops any test first, then writes LOAD OFF
        repeatedly — a single write can be lost on a busy link, and this is the
        one command that must land."""
        self.cancel_rtest()
        if self.load is None:
            return
        for _ in range(3):
            await self.load.set_load(False)
            await self.load.set_point(0.0)

    def estop(self):
        self.submit(self._estop())

    def cancel_rtest(self):
        t = self.rtest_task
        if t is not None and not t.done():
            self.loop.call_soon_threadsafe(t.cancel)

    # ---- resistance sweep -------------------------------------------------
    def start_rtest(self, fuse, sweep_s, start_a, peak_a):
        self.rtest_task = asyncio.run_coroutine_threadsafe(
            self._rtest(fuse, sweep_s, start_a, peak_a), self.loop)

        def done(f):
            try:
                f.result()
            except asyncio.CancelledError:
                self.emit("rtest_error", "Stopped")
            except Exception as e:                      # noqa: BLE001
                self.emit("rtest_error", "%s: %s" % (type(e).__name__, e))
        self.rtest_task.add_done_callback(done)

    async def _rtest(self, fuse, sweep_s, start_a, peak_a):
        load = self.load
        if load is None:
            self.emit("rtest_error", "Not connected")
            return
        try:
            await self._rtest_body(load, fuse, sweep_s, start_a, peak_a)
        finally:
            # Whatever happened - finished, aborted, cancelled - the load stops.
            await load.set_load(False)
            await load.set_point(0.0)

    async def _rtest_body(self, load, fuse, sweep_s, start_a, peak_a):
        self.emit("rtest_phase", "Priming…")
        await load.set_mode(MODE_CC)
        await load.set_point(0.0)
        await load.set_load(False)
        await load.pump(1.2)
        st = load.last
        if st is None:
            self.emit("rtest_error", "No telemetry from the load"); return
        if st.warning:
            self.emit("rtest_error", "Load protection is active (%s)" % st.warning); return
        voc = st.voltage
        if not (0.1 < voc < MAX_VOLTAGE_V):
            self.emit("rtest_error", "Source at %.3f V is outside the load's range" % voc)
            return

        cap = min(fuse * SAFETY, MAX_CURRENT_A, MAX_POWER_W / voc)
        peak = min(peak_a, cap) if peak_a > 0 else cap
        start = max(min(start_a, peak - MIN_TEST_CURRENT), MIN_TEST_CURRENT)
        if peak - start < 2 * MIN_TEST_CURRENT:
            self.emit("rtest_error", "Current span too small for a fit"); return
        half = sweep_s / 2.0
        ramp_step = (peak - start) * (SETPOINT_S / half)

        fit = Fit()
        bins = [[0.0, 0.0, 0.0, 0, 0] for _ in range(NBINS)]
        held = last_i = last_t = None
        have = False
        off_target = repaired = dropouts = 0
        peak_w = 0.0
        tmin = tmax = None
        rows = []

        def accept(i, v, temp, fan, t):
            nonlocal last_i, last_t, have, peak_w, tmin, tmax
            fit.add(i, v)
            peak_w = max(peak_w, i * v)
            tmin = temp if tmin is None else min(tmin, temp)
            tmax = temp if tmax is None else max(tmax, temp)
            span = peak - start
            if span > 1e-6:
                k = max(0, min(NBINS - 1, int(((i - start) / span) * NBINS)))
                b = bins[k]
                b[0] += i; b[1] += v; b[2] += temp; b[3] += 1; b[4] = max(b[4], fan)
            last_i, last_t, have = i, t, True

        self.emit("rtest_phase", "Sweeping…")
        t0 = time.perf_counter()
        target = start
        await load.set_point(target)
        await load.set_load(True)
        seen = len(load.packets)
        next_set = time.perf_counter() + SETPOINT_S
        last_on = time.perf_counter()

        while True:
            el = time.perf_counter() - t0
            if el >= sweep_s:
                break
            if time.perf_counter() >= next_set:
                next_set += SETPOINT_S
                f = el / half if el < half else 2 - el / half
                target = max(start + (peak - start) * max(0.0, min(1.0, f)), MIN_TEST_CURRENT)
                await load.set_point(target)
            await load.poll()

            while seen < len(load.packets):
                p = load.packets[seen]; seen += 1
                if not p.valid:
                    continue
                rows.append((p.t, target, p.voltage, p.current, p.temperature,
                             p.fan, int(p.load_on), p.raw))
                if p.warning:
                    self.emit("rtest_error", "Protection tripped (%s)" % p.warning)
                    return
                if not p.load_on:
                    dropouts += 1
                    # Writing LOAD_ON once is not enough on this hardware.
                    if time.perf_counter() - last_on > 0.400:
                        last_on = time.perf_counter()
                        await load.set_point(target)
                        await load.set_load(True)
                    continue

                # Hold every reading one packet so both neighbours are in hand.
                if held is not None:
                    h = held
                    held = None
                    rel = h[0]
                    near = (rel <= 3 * SETPOINT_S or rel >= sweep_s - 3 * SETPOINT_S
                            or abs(rel - half) <= 3 * SETPOINT_S)
                    sus = abs(h[3] - h[1]) > off_target_limit(h[1], ramp_step)
                    if not sus and have and not near:
                        stick = max(h[3] - max(last_i, p.current),
                                    min(last_i, p.current) - h[3], 0.0)
                        sus = stick > SPIKE_A
                    if not sus:
                        accept(h[3], h[2], h[4], h[5], h[0])
                    else:
                        fixed = None
                        if have:
                            dt = (time.perf_counter() - t0) - last_t
                            pred = (last_i if dt <= 0 else
                                    last_i + (p.current - last_i) * ((h[0] - last_t) / dt))
                            fixed = repair_off_target(h[3], pred)
                        if fixed is None:
                            off_target += 1
                        else:
                            repaired += 1
                            accept(fixed, h[2], h[4], h[5], h[0])
                held = (time.perf_counter() - t0, target, p.voltage, p.current,
                        p.temperature, p.fan)

            res = fit.result()
            self.emit("rtest_progress", {
                "elapsed": el, "total": sweep_s, "target": target,
                "n": fit.n, "R": res[0] if res else None,
                "off_target": off_target, "repaired": repaired,
            })
            await asyncio.sleep(POLL_S)

        if held is not None:
            h = held
            if abs(h[3] - h[1]) > off_target_limit(h[1], ramp_step):
                off_target += 1
            else:
                accept(h[3], h[2], h[4], h[5], h[0])

        await load.set_load(False)
        await load.set_point(0.0)

        res = fit.result()
        if res is None:
            self.emit("rtest_error", "Not enough usable samples for a fit"); return
        R, Voc, r2, se = res
        curve = [(b[0] / b[3], b[1] / b[3], b[2] / b[3], b[4]) for b in bins if b[3]]
        self.emit("rtest_done", {
            "R": R, "Voc": Voc, "r2": r2, "se": se, "n": fit.n,
            "imin": fit.min_i, "imax": fit.max_i, "sag": fit.max_v - fit.min_v,
            "peak_w": peak_w, "tmin": tmin or 0.0, "tmax": tmax or 0.0,
            "dropouts": dropouts, "off_target": off_target, "repaired": repaired,
            "curve": curve, "rows": rows, "fuse": fuse, "peak": peak,
            "start": start, "sweep_s": sweep_s,
            "reliable": fit.n >= 20 and (fit.max_i - fit.min_i) > 0.05
                        and (se <= 0.005 or (R > 1e-4 and se / R <= 0.05)),
        })


# ---------------------------------------------------------------------------
# A small canvas chart: a rolling strip for the monitor, a scatter for results.
# ---------------------------------------------------------------------------
class Chart(tk.Canvas):
    def __init__(self, master, **kw):
        super().__init__(master, bg=PANEL, highlightthickness=0, **kw)
        self.series = {}
        self.bind("<Configure>", lambda _e: self.redraw())

    def set_series(self, name, points, colour, unit=""):
        self.series[name] = (points, colour, unit)
        self.redraw()

    def clear(self):
        self.series.clear()
        self.redraw()

    def redraw(self):
        self.delete("all")
        w, h = self.winfo_width(), self.winfo_height()
        if w < 40 or h < 30:
            return
        pad_l, pad_r, pad_t, pad_b = 46, 10, 12, 22
        x0, y0, x1, y1 = pad_l, pad_t, w - pad_r, h - pad_b
        self.create_rectangle(x0, y0, x1, y1, outline="#2a3441")
        for k in range(1, 4):
            gy = y0 + (y1 - y0) * k / 4
            self.create_line(x0, gy, x1, gy, fill="#212936")
        live = [(n, s) for n, s in self.series.items() if len(s[0]) > 1]
        if not live:
            self.create_text((x0 + x1) / 2, (y0 + y1) / 2, text="no data",
                             fill=MUTED, font=("Segoe UI", 9))
            return
        for idx, (name, (pts, colour, unit)) in enumerate(live):
            xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
            xmin, xmax = min(xs), max(xs)
            ymin, ymax = min(ys), max(ys)
            if xmax - xmin < 1e-9:
                xmax = xmin + 1
            pad = max((ymax - ymin) * 0.12, abs(ymax) * 0.02, 1e-4)
            ymin -= pad; ymax += pad
            coords = []
            for px, py in pts:
                coords.append(x0 + (px - xmin) / (xmax - xmin) * (x1 - x0))
                coords.append(y1 - (py - ymin) / (ymax - ymin) * (y1 - y0))
            self.create_line(*coords, fill=colour, width=2, smooth=False)
            # Each series labels its own range, since they share the frame.
            self.create_text(x0 + 6, y0 + 10 + idx * 13, anchor="w",
                             text="%s %.3f%s" % (name, pts[-1][1], unit),
                             fill=colour, font=("Consolas", 9))
            if idx == 0:
                for k in range(5):
                    v = ymin + (ymax - ymin) * k / 4
                    gy = y1 - (y1 - y0) * k / 4
                    self.create_text(x0 - 5, gy, anchor="e", text="%.2f" % v,
                                     fill=MUTED, font=("Consolas", 8))

    def scatter(self, points, fit_line=None):
        """V against I, with the fitted line — the R-test result view."""
        self.delete("all")
        w, h = self.winfo_width(), self.winfo_height()
        if w < 40 or h < 30 or len(points) < 2:
            return
        pad_l, pad_r, pad_t, pad_b = 52, 12, 14, 30
        x0, y0, x1, y1 = pad_l, pad_t, w - pad_r, h - pad_b
        xs = [p[0] for p in points]; ys = [p[1] for p in points]
        xmin, xmax = 0.0, max(xs) * 1.05
        ymin, ymax = min(ys), max(ys)
        pad = max((ymax - ymin) * 0.15, 0.02)
        ymin -= pad; ymax += pad
        self.create_rectangle(x0, y0, x1, y1, outline="#2a3441")

        def sx(v):
            return x0 + (v - xmin) / (xmax - xmin) * (x1 - x0)

        def sy(v):
            return y1 - (v - ymin) / (ymax - ymin) * (y1 - y0)

        for k in range(5):
            gy = y0 + (y1 - y0) * k / 4
            self.create_line(x0, gy, x1, gy, fill="#212936")
            self.create_text(x0 - 5, gy, anchor="e", fill=MUTED, font=("Consolas", 8),
                             text="%.3f" % (ymax - (ymax - ymin) * k / 4))
            gx = x0 + (x1 - x0) * k / 4
            self.create_text(gx, y1 + 12, fill=MUTED, font=("Consolas", 8),
                             text="%.2f" % (xmin + (xmax - xmin) * k / 4))
        if fit_line:
            self.create_line(sx(xmin), sy(fit_line(xmin)), sx(xmax), sy(fit_line(xmax)),
                             fill=AMBER, width=2)
        for px, py in points:
            X, Y = sx(px), sy(py)
            self.create_oval(X - 3, Y - 3, X + 3, Y + 3, fill=BLUE, outline="")
        self.create_text((x0 + x1) / 2, y1 + 24, text="Current (A)", fill=MUTED,
                         font=("Segoe UI", 8))


# ---------------------------------------------------------------------------
# The window.
# ---------------------------------------------------------------------------
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("EL15 Load Control")
        self.geometry("1060x720")
        self.minsize(900, 620)
        self.configure(bg=BG)
        self.events = queue.Queue()
        self.worker = Worker(lambda kind, payload: self.events.put((kind, payload)))
        self.worker.start()

        self.address = DEFAULT_ADDR
        self.connected = False
        self.last = None
        self.load_on = False
        self.rtest_running = False
        self.result = None
        self.wave = []                     # (t, V, I) for the strip chart
        self.t0 = time.perf_counter()

        self._build()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(50, self._drain)

    # ---- layout -----------------------------------------------------------
    def _build(self):
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("TNotebook", background=BG, borderwidth=0)
        style.configure("TNotebook.Tab", background=PANEL, foreground=INK, padding=(14, 6))
        style.map("TNotebook.Tab", background=[("selected", "#26303d")])
        style.configure("TFrame", background=BG)
        style.configure("TLabel", background=BG, foreground=INK)
        style.configure("TEntry", fieldbackground=PANEL, foreground=INK)
        style.configure("TCombobox", fieldbackground=PANEL, foreground=INK)

        top = tk.Frame(self, bg=BG)
        top.pack(fill="x", padx=12, pady=(10, 6))
        self.dot = tk.Canvas(top, width=12, height=12, bg=BG, highlightthickness=0)
        self.dot.pack(side="left")
        self.dot_id = self.dot.create_oval(2, 2, 10, 10, fill=RED, outline="")
        self.conn_label = tk.Label(top, text="Disconnected", bg=BG, fg=INK,
                                   font=("Segoe UI", 11, "bold"))
        self.conn_label.pack(side="left", padx=(8, 16))
        self.conn_btn = tk.Button(top, text="Connect", command=self.on_connect,
                                  bg="#2a3441", fg=INK, relief="flat", width=12)
        self.conn_btn.pack(side="left")
        tk.Button(top, text="Scan", command=self.worker.scan, bg="#2a3441", fg=INK,
                  relief="flat", width=8).pack(side="left", padx=6)
        self.estop_btn = tk.Button(top, text="EMERGENCY STOP", command=self.on_estop,
                                   bg=RED, fg="white", relief="flat",
                                   font=("Segoe UI", 10, "bold"))
        self.estop_btn.pack(side="right")

        body = tk.Frame(self, bg=BG)
        body.pack(fill="both", expand=True, padx=12, pady=6)
        left = tk.Frame(body, bg=BG, width=330)
        left.pack(side="left", fill="y")
        left.pack_propagate(False)
        self._build_readouts(left)
        self._build_controls(left)

        nb = ttk.Notebook(body)
        nb.pack(side="left", fill="both", expand=True, padx=(12, 0))
        self.tab_monitor = tk.Frame(nb, bg=BG)
        self.tab_rtest = tk.Frame(nb, bg=BG)
        nb.add(self.tab_monitor, text="  Monitor  ")
        nb.add(self.tab_rtest, text="  R-Test  ")
        self._build_monitor(self.tab_monitor)
        self._build_rtest(self.tab_rtest)

        self.status = tk.Label(self, text="Ready", bg=PANEL, fg=MUTED, anchor="w",
                               font=("Segoe UI", 9))
        self.status.pack(fill="x", side="bottom", ipady=4)

    def _panel(self, parent, title):
        f = tk.LabelFrame(parent, text=title, bg=PANEL, fg=MUTED, bd=0,
                          font=("Segoe UI", 9), labelanchor="nw", padx=10, pady=8)
        f.pack(fill="x", pady=(0, 10))
        return f

    def _build_readouts(self, parent):
        f = self._panel(parent, "READINGS")
        self.v_var = tk.StringVar(value="—")
        self.i_var = tk.StringVar(value="—")
        self.p_var = tk.StringVar(value="—")
        for var, unit, colour in ((self.v_var, "V", GREEN), (self.i_var, "A", AMBER),
                                  (self.p_var, "W", BLUE)):
            row = tk.Frame(f, bg=PANEL)
            row.pack(fill="x")
            tk.Label(row, textvariable=var, bg=PANEL, fg=colour,
                     font=("Consolas", 26, "bold"), anchor="e", width=9).pack(side="left")
            tk.Label(row, text=unit, bg=PANEL, fg=MUTED,
                     font=("Segoe UI", 12)).pack(side="left", padx=(6, 0), pady=(10, 0))
        self.extra = tk.Label(f, text="", bg=PANEL, fg=MUTED, justify="left",
                              anchor="w", font=("Consolas", 9))
        self.extra.pack(fill="x", pady=(6, 0))
        self.warn = tk.Label(f, text="", bg=PANEL, fg=RED, anchor="w",
                             font=("Segoe UI", 10, "bold"))
        self.warn.pack(fill="x")

    def _build_controls(self, parent):
        f = self._panel(parent, "CONTROL")
        row = tk.Frame(f, bg=PANEL); row.pack(fill="x", pady=(0, 6))
        tk.Label(row, text="Mode", bg=PANEL, fg=MUTED, width=8, anchor="w").pack(side="left")
        self.mode_var = tk.StringVar(value="CC")
        cb = ttk.Combobox(row, textvariable=self.mode_var, state="readonly", width=10,
                          values=[m[1] for m in SETTABLE])
        cb.pack(side="left")
        cb.bind("<<ComboboxSelected>>", self.on_mode)

        row = tk.Frame(f, bg=PANEL); row.pack(fill="x", pady=(0, 6))
        self.sp_label = tk.Label(row, text="Set (A)", bg=PANEL, fg=MUTED, width=8, anchor="w")
        self.sp_label.pack(side="left")
        self.sp_entry = tk.Entry(row, width=10, bg="#0f141a", fg=INK, insertbackground=INK,
                                 relief="flat")
        self.sp_entry.pack(side="left")
        self.sp_entry.insert(0, "0.500")
        tk.Button(row, text="Set", command=self.on_setpoint, bg="#2a3441", fg=INK,
                  relief="flat", width=6).pack(side="left", padx=6)

        self.load_btn = tk.Button(f, text="LOAD ON", command=self.on_load_toggle,
                                  bg=GREEN, fg="white", relief="flat",
                                  font=("Segoe UI", 11, "bold"))
        self.load_btn.pack(fill="x", pady=(4, 0))

    def _build_monitor(self, parent):
        self.strip = Chart(parent, height=260)
        self.strip.pack(fill="both", expand=True, pady=(8, 6))
        bar = tk.Frame(parent, bg=BG); bar.pack(fill="x")
        tk.Button(bar, text="Clear", command=self.on_clear_wave, bg="#2a3441", fg=INK,
                  relief="flat", width=10).pack(side="left")
        tk.Button(bar, text="Export CSV…", command=self.on_export_wave, bg="#2a3441",
                  fg=INK, relief="flat", width=14).pack(side="left", padx=6)
        self.packet = tk.Label(parent, text="", bg=BG, fg=MUTED, anchor="w",
                               font=("Consolas", 8), wraplength=640, justify="left")
        self.packet.pack(fill="x", pady=(8, 0))

    def _build_rtest(self, parent):
        setup = tk.Frame(parent, bg=BG); setup.pack(fill="x", pady=(10, 6))
        self.rt_vars = {}
        for label, key, default in (("Fuse (A)", "fuse", "5"), ("Sweep (s)", "sweep", "30"),
                                    ("Start (A)", "start", "0.05"), ("Peak (A)", "peak", "0")):
            tk.Label(setup, text=label, bg=BG, fg=MUTED).pack(side="left", padx=(0, 4))
            e = tk.Entry(setup, width=7, bg=PANEL, fg=INK, insertbackground=INK, relief="flat")
            e.insert(0, default)
            e.pack(side="left", padx=(0, 12))
            self.rt_vars[key] = e
        self.rt_btn = tk.Button(setup, text="Start sweep", command=self.on_rtest,
                                bg=AMBER, fg="#141414", relief="flat",
                                font=("Segoe UI", 10, "bold"), width=14)
        self.rt_btn.pack(side="left")
        tk.Label(parent, text="Peak 0 = use the whole headroom the fuse allows",
                 bg=BG, fg=MUTED, font=("Segoe UI", 8)).pack(anchor="w")

        self.rt_progress = ttk.Progressbar(parent, maximum=100)
        self.rt_progress.pack(fill="x", pady=(8, 4))
        self.rt_phase = tk.Label(parent, text="Idle", bg=BG, fg=MUTED, anchor="w",
                                 font=("Consolas", 9))
        self.rt_phase.pack(fill="x")

        self.rt_head = tk.Label(parent, text="", bg=BG, fg=INK, anchor="w",
                                font=("Consolas", 15, "bold"))
        self.rt_head.pack(fill="x", pady=(8, 0))
        self.rt_detail = tk.Label(parent, text="", bg=BG, fg=MUTED, anchor="w",
                                  justify="left", font=("Consolas", 9))
        self.rt_detail.pack(fill="x")
        self.rt_chart = Chart(parent, height=200)
        self.rt_chart.pack(fill="both", expand=True, pady=(6, 6))
        tk.Button(parent, text="Save report CSV…", command=self.on_save_report,
                  bg="#2a3441", fg=INK, relief="flat", width=18).pack(anchor="w")

    # ---- event pump -------------------------------------------------------
    def _drain(self):
        try:
            while True:
                kind, payload = self.events.get_nowait()
                self._handle(kind, payload)
        except queue.Empty:
            pass
        self.after(50, self._drain)

    def _handle(self, kind, payload):
        if kind == "status":
            self._on_status(payload)
        elif kind == "connected":
            self.connected = True
            self.conn_label.config(text="Connected")
            self.conn_btn.config(text="Disconnect")
            self.dot.itemconfig(self.dot_id, fill=GREEN)
            self.status.config(text="Connected to %s" % payload)
        elif kind == "disconnected":
            self.connected = False
            self.rtest_running = False
            self.rt_btn.config(text="Start sweep")
            self.conn_label.config(text="Disconnected")
            self.conn_btn.config(text="Connect")
            self.dot.itemconfig(self.dot_id, fill=RED)
            self.status.config(text="Disconnected — load off")
        elif kind == "scanning":
            self.status.config(text="Scanning…")
        elif kind == "found":
            if payload:
                self.address = payload
                self.status.config(text="Found %s" % payload)
            else:
                self.status.config(text="No EL15 found")
        elif kind == "error":
            self.status.config(text=payload)
        elif kind == "rtest_phase":
            self.rt_phase.config(text=payload)
        elif kind == "rtest_progress":
            self._rtest_progress(payload)
        elif kind == "rtest_done":
            self._rtest_done(payload)
        elif kind == "rtest_error":
            self.rtest_running = False
            self.rt_btn.config(text="Start sweep")
            self.rt_phase.config(text=payload)
            self.rt_progress["value"] = 0

    def _on_status(self, st):
        self.last = st
        if not st.valid:
            return
        self.load_on = st.load_on
        self.v_var.set("%.3f" % st.voltage)
        self.i_var.set("%.3f" % st.current)
        self.p_var.set("%.1f" % (st.voltage * st.current))
        self.extra.config(text="mode %-5s  fan %d/5  %.1f °C  set %.3f\nload %s   %s"
                          % (st.mode_name, st.fan, st.temperature, st.setpoint,
                             "ON" if st.load_on else "off",
                             "locked" if st.lock_on else ""))
        self.warn.config(text=("⚠ PROTECTION: %s" % st.warning) if st.warning else "")
        self.load_btn.config(text="LOAD OFF" if st.load_on else "LOAD ON",
                             bg=RED if st.load_on else GREEN)
        t = time.perf_counter() - self.t0
        self.wave.append((t, st.voltage, st.current))
        if len(self.wave) > 900:
            del self.wave[:len(self.wave) - 900]
        self.strip.set_series("V", [(p[0], p[1]) for p in self.wave], GREEN, "V")
        self.strip.set_series("I", [(p[0], p[2]) for p in self.wave], AMBER, "A")
        self.packet.config(text=st.raw)

    # ---- actions ----------------------------------------------------------
    def on_connect(self):
        if self.connected:
            self.worker.disconnect()
        else:
            if not self.address:
                self.status.config(text="Scan for a device first")
                return
            self.status.config(text="Connecting to %s…" % self.address)
            self.worker.connect(self.address)

    def on_estop(self):
        self.worker.estop()
        self.rtest_running = False
        self.rt_btn.config(text="Start sweep")
        self.status.config(text="EMERGENCY STOP — load off")

    def on_mode(self, _e=None):
        name = self.mode_var.get()
        for code, label, unit, dp in SETTABLE:
            if label == name:
                self.sp_label.config(text="Set (%s)" % unit)
                self.worker.set_mode(code)
                return

    def on_setpoint(self):
        try:
            v = float(self.sp_entry.get())
        except ValueError:
            self.status.config(text="Setpoint must be a number")
            return
        self.worker.set_point(v)
        self.status.config(text="Setpoint → %.3f" % v)

    def on_load_toggle(self):
        if not self.connected:
            self.status.config(text="Not connected")
            return
        self.worker.set_load(not self.load_on)

    def on_clear_wave(self):
        self.wave.clear()
        self.strip.clear()

    def on_export_wave(self):
        if not self.wave:
            self.status.config(text="Nothing recorded yet")
            return
        path = filedialog.asksaveasfilename(defaultextension=".csv",
                                            initialfile="el15-monitor.csv",
                                            filetypes=[("CSV", "*.csv")])
        if not path:
            return
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["elapsed_s", "voltage_v", "current_a", "power_w"])
            for t, v, i in self.wave:
                w.writerow(["%.3f" % t, "%.4f" % v, "%.4f" % i, "%.4f" % (v * i)])
        self.status.config(text="Saved %s" % path)

    def on_rtest(self):
        if self.rtest_running:
            self.worker.cancel_rtest()
            return
        if not self.connected:
            self.status.config(text="Connect first")
            return
        try:
            fuse = float(self.rt_vars["fuse"].get())
            sweep = float(self.rt_vars["sweep"].get())
            start = float(self.rt_vars["start"].get())
            peak = float(self.rt_vars["peak"].get())
        except ValueError:
            self.status.config(text="R-Test settings must be numbers")
            return
        if fuse <= 0 or not (5 <= sweep <= 900):
            self.status.config(text="Fuse > 0, sweep 5-900 s")
            return
        cap = min(fuse * SAFETY, MAX_CURRENT_A)
        if not messagebox.askokcancel(
                "Start sweep",
                "This draws real current: a ramp up to about %.2f A and back, over %.0f s.\n\n"
                "Check what is connected before continuing." % (cap, sweep)):
            return
        self.result = None
        self.rt_chart.clear()
        self.rt_head.config(text="")
        self.rt_detail.config(text="")
        self.rtest_running = True
        self.rt_btn.config(text="Stop")
        self.worker.start_rtest(fuse, sweep, start, peak)

    def _rtest_progress(self, p):
        self.rt_progress["value"] = min(100.0, p["elapsed"] / p["total"] * 100.0)
        r = "R %.2f mΩ" % (p["R"] * 1000) if p["R"] else "R —"
        self.rt_phase.config(
            text="%5.1f/%.0f s   set %.3f A   n=%d   %s   off %d  fixed %d"
            % (p["elapsed"], p["total"], p["target"], p["n"], r,
               p["off_target"], p["repaired"]))

    def _rtest_done(self, r):
        self.result = r
        self.rtest_running = False
        self.rt_btn.config(text="Start sweep")
        self.rt_progress["value"] = 100
        self.rt_phase.config(text="Done")
        self.rt_head.config(
            text="%.2f mΩ  ± %.2f mΩ" % (r["R"] * 1000, r["se"] * 1000),
            fg=GREEN if r["reliable"] else AMBER)
        self.rt_detail.config(text=(
            "Voc %.4f V    R² %.5f    %s\n"
            "samples %d in %d bands    current %.4f → %.4f A    sag %.4f V\n"
            "peak %.2f W    temp %.1f → %.1f °C    dropouts %d\n"
            "off-target %d (not fitted)    recovered %d (corruption inverted)"
            % (r["Voc"], r["r2"], "reliable" if r["reliable"] else "low confidence",
               r["n"], len(r["curve"]), r["imin"], r["imax"], r["sag"],
               r["peak_w"], r["tmin"], r["tmax"], r["dropouts"],
               r["off_target"], r["repaired"])))
        pts = [(c[0], c[1]) for c in r["curve"]]
        self.rt_chart.scatter(pts, lambda i: r["Voc"] - r["R"] * i)

    def on_save_report(self):
        if not self.result:
            self.status.config(text="No result to save")
            return
        r = self.result
        path = filedialog.asksaveasfilename(defaultextension=".csv",
                                            initialfile="RTEST.csv",
                                            filetypes=[("CSV", "*.csv")])
        if not path:
            return
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["# EL15 circuit resistance test (desktop)"])
            w.writerow(["# Fuse rating (A)", "%.2f" % r["fuse"]])
            w.writerow(["# Sweep start current (A)", "%.3f" % r["start"]])
            w.writerow(["# Sweep peak current (A)", "%.3f" % r["peak"]])
            w.writerow(["# Sweep duration (s)", "%.0f" % r["sweep_s"]])
            w.writerow([])
            w.writerow(["band", "current_a", "voltage_v", "power_w", "temperature_c", "fan"])
            for k, c in enumerate(r["curve"], 1):
                w.writerow([k, "%.4f" % c[0], "%.4f" % c[1], "%.3f" % (c[0] * c[1]),
                            "%.1f" % c[2], c[3]])
            w.writerow([])
            w.writerow(["quantity", "value", "unit"])
            for name, val, unit in (("resistance", "%.6f" % r["R"], "ohm"),
                                    ("resistance_std_err", "%.6f" % r["se"], "ohm"),
                                    ("open_circuit_voltage", "%.4f" % r["Voc"], "V"),
                                    ("r_squared", "%.5f" % r["r2"], ""),
                                    ("raw_samples", r["n"], ""),
                                    ("current_min", "%.4f" % r["imin"], "A"),
                                    ("current_max", "%.4f" % r["imax"], "A"),
                                    ("voltage_sag", "%.4f" % r["sag"], "V"),
                                    ("peak_power", "%.3f" % r["peak_w"], "W"),
                                    ("temperature_min", "%.1f" % r["tmin"], "C"),
                                    ("temperature_max", "%.1f" % r["tmax"], "C"),
                                    ("load_dropouts", r["dropouts"], ""),
                                    ("off_target_samples", r["off_target"], ""),
                                    ("repaired_samples", r["repaired"], ""),
                                    ("reliable", "yes" if r["reliable"] else "no", "")):
                w.writerow([name, val, unit])
            w.writerow([])
            w.writerow(["elapsed_s", "target_a", "voltage_v", "current_a",
                        "temperature_c", "fan", "load_on", "raw"])
            for row in r["rows"]:
                w.writerow(["%.4f" % row[0], "%.3f" % row[1], "%.4f" % row[2],
                            "%.4f" % row[3], "%.1f" % row[4], row[5], row[6], row[7]])
        self.status.config(text="Saved %s" % path)

    # ---- teardown ---------------------------------------------------------
    def on_close(self):
        """Never leave the load sinking current because a window closed."""
        if self.connected:
            self.status.config(text="Shutting the load down…")
            self.update_idletasks()
            self.worker.estop()
            self.worker.disconnect()
            # Give the teardown a moment to reach the device before the loop dies.
            deadline = time.time() + 2.0
            while self.connected and time.time() < deadline:
                self.update()
                time.sleep(0.05)
        self.worker.loop.call_soon_threadsafe(self.worker.loop.stop)
        self.destroy()


def selftest():
    """Build the whole UI, pump the event loop, tear it down. Catches the class
    of mistake that only shows up when Tk actually lays the window out."""
    app = App()
    app.update_idletasks()
    app.update()
    app._handle("rtest_progress", {"elapsed": 3.0, "total": 30.0, "target": 1.2,
                                   "n": 42, "R": 0.0851, "off_target": 0, "repaired": 1})
    app._handle("rtest_done", {
        "R": 0.0851, "Voc": 12.99, "r2": 0.987, "se": 0.00055, "n": 308,
        "imin": 0.05, "imax": 3.99, "sag": 0.36, "peak_w": 50.6, "tmin": 30.0,
        "tmax": 41.0, "dropouts": 0, "off_target": 1, "repaired": 1, "reliable": True,
        "curve": [(0.1 * k, 13.0 - 0.085 * 0.1 * k, 30.0, 0) for k in range(1, 33)],
        "rows": [], "fuse": 5.0, "peak": 4.0, "start": 0.05, "sweep_s": 30.0})
    app.update()
    app.worker.loop.call_soon_threadsafe(app.worker.loop.stop)
    app.destroy()
    _say("selftest OK")


def _say(line):
    """Report a check result.

    A windowed PyInstaller build has no console — `print` there goes nowhere —
    so checks also append to a log beside the executable. Without this a frozen
    build can only be tested by looking at it, which is no test at all.
    """
    print(line)
    try:
        base = Path(sys.executable if getattr(sys, "frozen", False) else __file__)
        with open(base.parent / "el15_check.log", "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def link_check():
    """Drive the app's own Worker against the real load, with no UI.

    Exercises the part a UI selftest cannot: the asyncio-thread bridge, the
    session lifecycle, and that the teardown actually reaches the device. Draws
    no current — it never turns the load on.
    """
    seen = []
    done = threading.Event()

    def emit(kind, payload):
        if kind == "status":
            if payload.valid:
                seen.append(payload)
            return
        _say("  %-13s %s" % (kind, payload if kind != "found" else payload))
        if kind == "disconnected":
            done.set()

    w = Worker(emit)
    w.start()
    addr = DEFAULT_ADDR
    _say("connecting to %s" % addr)
    w.connect(addr)
    # A BLE connect on this stack takes the better part of ten seconds; wait for
    # telemetry rather than for a stopwatch.
    deadline = time.time() + 25.0
    while time.time() < deadline and len(seen) < 20:
        time.sleep(0.25)
    if not seen:
        _say("FAIL: no telemetry"); return 1
    st = seen[-1]
    _say("  %d valid packets, last: %.4f V  %.4f A  %.1f C  load %s  mode %s"
         % (len(seen), st.voltage, st.current, st.temperature,
            "ON" if st.load_on else "off", st.mode_name))
    w.disconnect()
    done.wait(timeout=6.0)
    w.loop.call_soon_threadsafe(w.loop.stop)
    ok = bool(seen) and not seen[-1].load_on
    _say("link check %s" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


def rtest_check(sweep_s=15.0):
    """Run the app's OWN sweep coroutine against the real load, no UI.

    The sweep loop in this file is a transcription of the bench engine, and a
    transcription is exactly where a bug hides — so it gets exercised on
    hardware rather than assumed. THIS DRAWS REAL CURRENT.
    """
    out = {}
    done = threading.Event()

    def emit(kind, payload):
        if kind == "status":
            return
        if kind in ("rtest_done", "rtest_error"):
            out[kind] = payload
            done.set()
        elif kind == "rtest_progress":
            print("\r  %5.1f/%.0f s  set %.3f A  n=%d  off %d fixed %d"
                  % (payload["elapsed"], payload["total"], payload["target"],
                     payload["n"], payload["off_target"], payload["repaired"]),
                  end="")
        else:
            print("  %-13s %s" % (kind, payload))

    w = Worker(emit)
    w.start()
    w.connect(DEFAULT_ADDR)
    deadline = time.time() + 25.0
    while time.time() < deadline and w.load is None:
        time.sleep(0.25)
    if w.load is None:
        print("FAIL: never connected"); return 1
    w.start_rtest(5.0, sweep_s, 0.05, 0.0)
    done.wait(timeout=sweep_s + 40)
    print()
    w.disconnect()
    time.sleep(3.0)
    w.loop.call_soon_threadsafe(w.loop.stop)
    if "rtest_error" in out:
        print("FAILED: %s" % out["rtest_error"]); return 1
    r = out.get("rtest_done")
    if not r:
        print("FAILED: no result"); return 1
    print("R %.2f mOhm +/- %.2f   Voc %.4f   R2 %.5f   n=%d   off %d   fixed %d   %s"
          % (r["R"] * 1000, r["se"] * 1000, r["Voc"], r["r2"], r["n"],
             r["off_target"], r["repaired"],
             "reliable" if r["reliable"] else "low confidence"))
    print("bands %d   current %.4f..%.4f A   peak %.2f W"
          % (len(r["curve"]), r["imin"], r["imax"], r["peak_w"]))
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        selftest()
    elif "--check" in sys.argv:
        sys.exit(link_check())
    elif "--rtest-check" in sys.argv:
        sys.exit(rtest_check())
    else:
        App().mainloop()
