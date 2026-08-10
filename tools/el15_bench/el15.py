"""Minimal EL15 BLE client for bench debugging.

A faithful port of the app's El15Protocol / El15BleManager rules:
  - sum-to-zero checksum on every command frame (the device silently drops
    frames that do not sum to 0 mod 256)
  - control writes paced >= 50 ms apart, polls held 20 ms behind a control
    write (no-response writes landing too close are dropped by the device)
  - 28-byte frame reassembly with header resync

Difference from the app, and the reason this exists: EVERY frame is kept as raw
hex alongside its decode, so a glitched packet can be taken apart byte by byte.

SAFETY: use with `async with El15(addr) as load:` — the context manager writes
LOAD OFF + setpoint 0 on the way out, including on an exception or Ctrl-C.
"""
import asyncio
import os
import struct
import time

from bleak import BleakClient, BleakScanner

SERVICE = "0000fff0-0000-1000-8000-00805f9b34fb"
NOTIFY = "0000fff1-0000-1000-8000-00805f9b34fb"
WRITE = "0000fff3-0000-1000-8000-00805f9b34fb"

# The unit this was written against. Override with EL15_ADDR, or pass None to
# find() to scan. (An EL15 advertises FFF0 and the name EL15_BLE.)
DEFAULT_ADDR = os.environ.get("EL15_ADDR", "B5:F2:A1:E5:63:A7")

HEADER = bytes([0xDF, 0x07, 0x03, 0x08])
FRAME_LEN = 28

MODE_CC = 0x01
MODE_NAMES = {0x01: "CC", 0x02: "CAP", 0x09: "CV", 0x0A: "DCR", 0x11: "CR", 0x19: "CP"}

CTRL_GAP = 0.050        # s between two control writes
CTRL_POLL_GAP = 0.020   # s a poll waits behind a control write

MAX_CURRENT_A = 12.0
MAX_POWER_W = 150.0
MAX_VOLTAGE_V = 60.0


def checksum(frame: bytes) -> bytes:
    return frame + bytes([(0x100 - (sum(frame) & 0xFF)) & 0xFF])


POLL = checksum(bytes([0xAF, 0x07, 0x03, 0x08, 0x00]))
LOAD_ON = checksum(bytes([0xAF, 0x07, 0x03, 0x09, 0x01, 0x04]))
LOAD_OFF = checksum(bytes([0xAF, 0x07, 0x03, 0x09, 0x01, 0x00]))


def mode_cmd(mode: int) -> bytes:
    return checksum(bytes([0xAF, 0x07, 0x03, 0x03, 0x01, mode]))


def setpoint_cmd(value: float) -> bytes:
    return checksum(bytes([0xAF, 0x07, 0x03, 0x04, 0x04]) + struct.pack("<f", value))


async def find(timeout=10.0):
    """Scan for an EL15 and return its address, or None.

    Matches on the FFF0 service or the advertised name. Note the phone app
    deliberately does NOT filter on FFF0 (some units omit it from the advert);
    here a strict match is fine because a bench run can simply be retried.
    """
    found = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for addr, (dev, adv) in found.items():
        name = (adv.local_name or dev.name or "")
        if SERVICE in [u.lower() for u in (adv.service_uuids or [])] or "EL15" in name.upper():
            print("found %s  %s  rssi %d" % (addr, name, adv.rssi))
            return addr
    return None


class Status:
    __slots__ = ("raw", "crc_ok", "valid", "voltage", "current", "power", "runtime",
                 "temperature", "setpoint", "mode", "mode_name", "fan", "load_on",
                 "lock_on", "warning", "t")

    def __repr__(self):
        return ("Status(V=%.4f I=%.4f T=%.1f set=%.3f fan=%d load=%s warn=%r)"
                % (self.voltage, self.current, self.temperature, self.setpoint,
                   self.fan, self.load_on, self.warning))


def parse(data: bytes, t: float) -> Status:
    s = Status()
    s.t = t
    s.raw = data.hex(" ").upper()
    s.crc_ok = (sum(data) & 0xFF) == 0
    s.valid = False
    s.voltage = s.current = s.power = s.temperature = s.setpoint = 0.0
    s.runtime = 0
    s.mode = 0
    s.mode_name = "?"
    s.fan = 0
    s.load_on = s.lock_on = False
    s.warning = ""
    if len(data) < FRAME_LEN or data[:4] != HEADER or not s.crc_ok:
        return s

    s.voltage = struct.unpack_from("<f", data, 7)[0]
    s.current = struct.unpack_from("<f", data, 11)[0]
    s.runtime = struct.unpack_from("<i", data, 15)[0]
    s.power = s.voltage * s.current

    b5, b6 = data[5], data[6]
    warn_flag = (b5 & 0x06) == 0x06
    raw_mode = (b5 & (0x1F & ~0x06)) if warn_flag else (b5 & 0x1F)
    mode = raw_mode if raw_mode in MODE_NAMES else (raw_mode | 0x01)
    s.mode = mode
    s.mode_name = MODE_NAMES.get(mode, "?%02X" % mode)
    if warn_flag:
        code = b6 >> 4
        s.warning = {0x6: "REV", 0x9: "UVP"}.get(code, "PROT %X" % code)
    if mode not in (0x02, 0x0A):
        s.temperature = struct.unpack_from("<f", data, 19)[0]
        s.setpoint = struct.unpack_from("<f", data, 23)[0]
    s.fan = (b5 >> 6) | ((b6 & 0x01) << 2)
    s.load_on = bool(b6 & 0x02)
    s.lock_on = bool(b6 & 0x04)
    s.valid = True
    return s


class El15:
    def __init__(self, address, on_status=None, verbose=True):
        self.address = address
        self.client = BleakClient(address)
        self.on_status = on_status
        self.verbose = verbose
        self.buf = b""
        self.last_ctrl = 0.0
        self.packets = []
        self.t0 = time.perf_counter()
        self._lock = asyncio.Lock()

    async def __aenter__(self):
        await self.client.connect()
        if self.verbose:
            print("connected to %s" % self.address)
        await self.client.start_notify(NOTIFY, self._notify)
        return self

    async def __aexit__(self, *exc):
        # Belt and braces: the load must never be left sinking current.
        try:
            for _ in range(3):
                await self._write(LOAD_OFF, ctrl=True)
                await self._write(setpoint_cmd(0.0), ctrl=True)
        except Exception as e:
            print("!! could not confirm LOAD OFF: %s" % e)
        try:
            await self.client.stop_notify(NOTIFY)
        except Exception:
            pass
        await self.client.disconnect()
        if self.verbose:
            print("disconnected (load off)")

    def _notify(self, _handle, data: bytearray):
        now = time.perf_counter() - self.t0
        buf = bytes(data) if bytes(data[:4]) == HEADER else self.buf + bytes(data)
        while len(buf) >= 4 and buf[:4] != HEADER:
            buf = buf[1:]
        while len(buf) >= FRAME_LEN:
            frame, buf = buf[:FRAME_LEN], buf[FRAME_LEN:]
            st = parse(frame, now)
            self.packets.append(st)
            if self.on_status:
                self.on_status(st)
        self.buf = buf if len(buf) < 128 else b""

    async def _write(self, frame: bytes, ctrl: bool):
        async with self._lock:
            now = time.perf_counter()
            gap = CTRL_GAP if ctrl else CTRL_POLL_GAP
            wait = self.last_ctrl + gap - now
            if wait > 0:
                await asyncio.sleep(wait)
            await self.client.write_gatt_char(WRITE, frame, response=False)
            if ctrl:
                self.last_ctrl = time.perf_counter()

    async def poll(self):
        await self._write(POLL, ctrl=False)

    async def set_mode(self, mode):
        await self._write(mode_cmd(mode), ctrl=True)

    async def set_point(self, amps):
        amps = max(0.0, min(float(amps), MAX_CURRENT_A))
        await self._write(setpoint_cmd(amps), ctrl=True)

    async def set_load(self, on):
        await self._write(LOAD_ON if on else LOAD_OFF, ctrl=True)

    async def pump(self, seconds, interval=0.05):
        """Poll for a while, letting notifications land."""
        end = time.perf_counter() + seconds
        while time.perf_counter() < end:
            await self.poll()
            await asyncio.sleep(interval)

    @property
    def last(self):
        for st in reversed(self.packets):
            if st.valid:
                return st
        return None
