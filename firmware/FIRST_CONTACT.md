# First Contact — Real EL15 Bench Checklist

Ordered procedure for working with the real ALIENTEK EL15 (150 W / 60 V / 12 A).
**BLOCKING** = do not proceed past a failure of this step. Work the list top to bottom.

Derived from a code audit on 2026-07-22 (protocol cross-checked byte-for-byte against the
DM40GUI reference, github.com/maj113/DM40GUI). Items marked *(fixed 2026-07-22)* refer to
holes found in that audit and already patched — listed so you know they were considered.

> ## Progress as of 2026-08-01
>
> **Phase 0 and Phase 1 are DONE** (session of 2026-07-24). Steps 1–14 passed on
> the real unit, with three defects found and fixed on the bench:
> the missing **command checksum** (step 9/10/11 all failed until every command
> frame was made to sum to zero — see `HANDOVER.md` §10), a **connect crash**
> from re-discovering GATT characteristics, and **dropped no-response writes**
> that now require ≥50 ms pacing between control commands. SD read/write was
> also proven in the same session.
>
> **Still to do: steps 15–16b (the safety drills) and all of Phase 2 (real
> current).** No current has yet been drawn on the real unit. Start at step 15.
>
> ## Update 2026-08-01
>
> **Step 17 (bench PSU, first real current) is DONE** — via the R-test's
> continuous sweep, at 0.3–0.5 A on a ~20 V bench source, repeating to better
> than 1 %. Steps 18–23 (real pack, capacity test) are still outstanding, and
> **no capacity test has ever drawn real current.**
>
> **New hazard, read before any bench work:** opening a serial monitor RESETS
> this board. Doing it mid-sweep rebooted the controller with ~1 A flowing and
> left the EL15 sinking until its own protection tripped. Attach the monitor
> BEFORE starting anything, or open the port with DTR/RTS both false. HANDOVER §1.
>
> Two corrections to the text below, from what was learned that day:
> **step 13 is obsolete** — the poll default is now **50 ms (20 Hz)**, which was
> measured as the device's practical maximum, so leave it there rather than at
> 500 ms; and in **step 23** the SD card no longer shares the panel's SPI bus, so
> there is no "shared-bus handover" to survive — just confirm the file.

## Phase 0 — before the EL15 is powered

1. **BLOCKING — Flash the current tip; keep a serial monitor attached all day.**
   Serial is the only forensic channel: `[boot] reset reason:`, `[ble] connecting to <addr>
   (addr type N)` / `connect() FAILED rc=`, `[ble] status frame DROPPED (checksum): ...`
   (rate-limited — added so a checksum-convention mismatch can't fail silently),
   `[guard] recovery ...`, `[btn] EMERGENCY STOP`, `[batt] done:/error:`.
   `pio device monitor -p COM4 -b 115200`. Confirm a clean boot, no panic/bootloop.

2. *(advisory)* **NTP clock sync FIRST, before ever connecting to the EL15.** A successful
   sync deliberately reboots the controller; do it at session start with nothing connected
   so CSV reports get real timestamps. The sync/scan buttons now refuse while the load is
   on *(fixed 2026-07-22)* — don't rely on that, just do it first.

3. **BLOCKING — Control run against the Android simulator.** Scan → connect → Monitor
   heroes updating → disconnect. A same-day green baseline means any real-unit failure is
   attributable to the device, not a firmware regression (a heap regression presents as
   HCI 0x3e "Connect failed" and would be indistinguishable otherwise).

4. **BLOCKING — Brief the bail-outs.** To stop current: the on-screen **LOAD OFF** or the
   **BOOT button** (hardware e-stop, works from any screen; stops both engines and pushes
   LOAD_OFF + setpoint 0). Disconnect now also pushes LOAD_OFF first whenever anything is
   hot *(fixed 2026-07-22)* — but keep the discipline: stop first, then disconnect.
   Physical lead removal is the true last resort; keep the pack leads reachable.
   Guard reconnect attempts freeze the UI ~4 s each — deliberate, don't power-cycle.

## Phase 1 — real EL15, input terminals OPEN (zero energy)

5. **BLOCKING — Zero-source setup.** EL15 mains-powered, nothing on its input terminals.
   LOAD ON at an open input sinks ~0 A; every sim-to-real difference gets flushed out here.

6. **BLOCKING — Scan finds it.** The firmware lists only *named* advertisers (active scan,
   so a scan-response name counts). The reference implementation matches names starting
   with "EL15", so it should appear. If it never does: verify with a phone (nRF Connect)
   whether it advertises unnamed — that would need a small firmware change first.

7. **BLOCKING — Connect; record the address type.** Serial prints
   `[ble] connecting to <addr> (addr type N)` — **write N and the MAC down** (the guard's
   post-crash reconnect depends on the stored type). Real unit is likely type 0 (public)
   and should connect first-attempt in <1 s. "Not an EL15 (no FFF0)" or "characteristics
   missing" = GATT differs from the reference capture — stop, dump the GATT with a phone.

8. **BLOCKING — Live telemetry.** V/I/mode/fan/runtime refreshing ~2×/s on Monitor. This is
   the never-tested MTU/chunking/checksum path (we request MTU 247; at MTU 23 the 28-byte
   frame arrives chunked and exercises reassembly). Blank Monitor while "Connected":
   watch serial for `status frame DROPPED (checksum)` — that distinguishes a checksum
   mismatch from frames not arriving at all.

9. **BLOCKING — Mode round-trip, CC first.** Pick CC/CV/CR/CP; Monitor badge AND the
   EL15's own front panel must agree. Both engines depend on CC (0x01) landing correctly.
   (Mode changes are now refused while a test runs — *fixed 2026-07-22*.)

10. **BLOCKING — Setpoint round-trip.** CC 0.500 A, then 1.234 A: the EL15 front panel and
    the Monitor Set field must both show the exact value (float32-LE encoding's first
    independent check).

11. **BLOCKING — LOAD ON/OFF echo.** CC 0.100 A, LOAD ON: device panel input ON, current
    ~0, button solid red + "SINKING". OFF returns everything. The load button reflects the
    DEVICE-reported bit; the guard arms from the same echo — if this is wrong, every
    safety layer above it is blind. (A protection warning can no longer block the OFF
    direction of this button — *fixed 2026-07-22*.)

12. *(advisory)* Glance at device CAP mode (parses a different packet layout), confirm
    nothing garbles, return to CC. Tomorrow's battery runs use the BATT screen (CC +
    local integration), never device CAP mode.

13. ~~*(advisory)* Leave the poll rate at the 500 ms default; probe faster rates only after
    everything below passes.~~ **Done and superseded 2026-07-24:** the rate was swept
    250→20 ms against the real unit. It produces **~17-19 fresh samples/s**, so **50 ms
    (20 Hz) is the practical maximum** and is now the default — faster only refetches
    repeated frames and floods the link (a 20 ms flood once dropped the board off USB
    entirely, needing a physical unplug). Leave it at 20 Hz.

14. **BLOCKING — BOOT e-stop dry run.** LOAD ON at open input → BOOT: serial
    `[btn] EMERGENCY STOP`, alarm, red banner, and the EL15 panel showing input OFF.
    (If disconnected at the time, the banner now honestly says the stop could NOT reach
    the load — *fixed 2026-07-22*.)

15. **BLOCKING — Link-guard hot-drop drill.** LOAD ON at open input, then kill the link
    (power the EL15 off / walk out of range). Expect the red "LOAD MAY STILL BE ON"
    banner + repeating alarm + attempt counter (banner now stays visible even over an
    open menu/keypad — *fixed 2026-07-22*). Restore the link mid-loop: expect
    `[guard] recovery succeeded`, "LOAD FORCED OFF", device input OFF. Also let one run
    exhaust all 8 attempts: the failure banner is now tappable to retry, and starting a
    manual scan/connect clears it *(fixed 2026-07-22)*.

16. *(advisory)* **Crash-recovery drill.** LOAD ON at open input, yank the controller's
    USB. On reboot: amber "restarted with the load ON — tap to reconnect and force LOAD
    OFF" offer; tap it; verify the device input goes OFF. First from-cold reconnect
    against the real unit's address type. If it fails, unattended runs are off the table.

16a. *(advisory)* **Load-safe power-off drill.** LOAD ON at open input, then **long-press
    PWR**: expect serial `[btn] POWER OFF (load stopped)`, "POWERING OFF — load stopped"
    banner, the EL15 input going OFF, then the controller powering down (on USB it may
    reset instead of fully powering off — expected). Confirms a power-off can't strand
    the load. *(new 2026-07-22, AXP2101)*

16b. *(advisory)* **Controller-brownout auto-safe.** Hard to bench without a low battery —
    but know the behavior: on battery (not USB), at ~8% / ~3.30 V the controller
    force-stops the load and shows an amber "CONTROLLER BATTERY CRITICAL" banner
    (`[pmic] ... CRITICAL ... forcing LOAD OFF` on serial). For any long unattended run,
    **keep the controller on USB** so this never has to fire. *(new 2026-07-22, AXP2101)*

## Phase 2 — first real current

17. *(advisory but strongly recommended)* **Bench PSU before any battery.** ~5 V with a
    1 A current limit bounds every failure mode. CC 0.5 A, LOAD ON, verify V/I/W track
    the panel. Then set the EL15's hardware UVP above the PSU voltage to provoke a REAL
    protection packet: warn banner latches, LOAD ON gated (OFF never is), engines abort
    with "Protection tripped". Real warning bytes have never been seen — only simulated.

18. **BLOCKING — Set the EL15's own hardware UVP before the first pack.** Firmware cutoff
    must sit ABOVE the hardware UVP (e.g. cutoff 3.4 V/cell → UVP 3.0 V/cell) so firmware
    cuts first and UVP is a pure backstop that needs no radio.

19. **BLOCKING — First pack: small, protected, partially discharged, SHORT run.** One
    protected 18650 at ~3.9 V resting: cutoff 3.6 V (high on purpose), 0.5 A, rest 60 s.
    Verify: priming holds load off ~1.5 s and reads Voc; discharge starts at 0.500 A on
    UI AND panel; Ah/Wh integrate; debounced cutoff ends it; rebound recorded; result +
    `[batt] done:` sane.

20. **BLOCKING — Mid-discharge STOP and BOOT e-stop with real current.** On short runs:
    STOP → device input OFF + partial result; BOOT → e-stop + input OFF. Verify on the
    EL15's own panel, not just the UI.

21. *(advisory)* **Mid-discharge Disconnect.** The LOAD_OFF-then-flush window before the
    link drops is 40 ms — never validated on the real module's write-no-response path.
    If the load stays on after a mid-test Disconnect, treat as must-fix.

22. **BLOCKING — Live guard drill at 0.5 A.** Walk out of range mid-discharge, return,
    confirm "LOAD FORCED OFF" and input OFF on the panel — hand on the leads throughout.
    **No unattended run is permitted until this passes.**

23. *(advisory)* **Save the first CSV.** Green `BATT_NNN.CSV` (or an honest red reason —
    "No card detected (reseat it)" / "Card not formatted (use FAT32)"), then open the CSV
    on a PC and confirm the header carries the probe wiring and a real RTC timestamp.
    The card is on its own bit-banged SPI pins now, so there is no bus handover for the
    display to survive — but the save still blocks the loop task for ~1 s, so expect the
    UI to sit on "Saving..." and then report. This UI Save path has **never been
    exercised**; it is the single highest-value untested item in the firmware.
    Also delete any leftover `SDTEST_00N.CSV` files from the SD bring-up session.
