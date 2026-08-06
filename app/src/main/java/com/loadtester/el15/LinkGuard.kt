package com.loadtester.el15

import android.content.Context
import android.os.Handler
import android.os.Looper

/**
 * Link-loss auto-stop supervisor + crash/relaunch recovery. Kotlin port of the
 * firmware's `link_guard.h`.
 *
 * The gap this closes: the EL15 keeps sinking current after the BLE link drops.
 * Nothing on the phone can stop it — every command goes over that link — so a
 * dropped connection mid-discharge means a battery keeps draining until the
 * load's own UVP catches it, or doesn't. The engines' stop() paths write
 * LOAD_OFF into a dead link and silently achieve nothing.
 *
 * So: whenever the load is (or might be) energised, the guard is ARMED, which
 * means two things.
 *   1. A dropped link starts a reconnect-and-kill loop: reconnect, push
 *      LOAD_OFF + setpoint 0 several times, and only then call it resolved.
 *      Every attempt is announced, and running out of attempts is reported as a
 *      failure to act on — not quietly dropped.
 *   2. The armed state is written to SharedPreferences *synchronously*
 *      ([Prefs.armInFlight]), so if the app is killed instead — by the user, by
 *      the OS reclaiming memory, by a crash — the next launch knows the load was
 *      left on and can offer the same reconnect-and-kill.
 *
 * Android differs from the firmware in one way worth noting: the firmware's
 * tick() blocks the loop task for the duration of a connect attempt, so it
 * deliberately shortens its connect timeout. Here [El15BleManager.reconnect] is
 * asynchronous, so the guard instead arms its own deadline per attempt and lets
 * the callbacks land normally. Nothing blocks the main thread.
 *
 * Main-thread confined.
 */
class LinkGuard(
    private val app: Context,
    private val ble: El15BleManager,
) {
    interface Listener {
        /**
         * Progress/outcome for the UI. [resolved] distinguishes "the load is off,
         * stand down" from "still trying".
         */
        fun onGuardAlert(title: String, message: String, resolved: Boolean)

        /**
         * Final give-up, after [MAX_ATTEMPTS]. Wired separately so the UI can show
         * an ACTIONABLE banner (tap = [retry]) instead of a permanently locked
         * one — a locked banner with no exit suppressed all later protection
         * alerts.
         */
        fun onGuardFailed(message: String)

        /** Audible warning, repeated per attempt. */
        fun onGuardAlarm() {}
    }

    var listener: Listener? = null

    private enum class State { IDLE, RECOVERING, FAILED }

    private val main = Handler(Looper.getMainLooper())
    private var state = State.IDLE
    private var armedWhat = Prefs.FLIGHT_NONE

    /** Latched by CONNECTED; a drop needs it set. */
    private var wasConnected = false
    private var attempts = 0
    private var attemptDeadline: Runnable? = null

    val armed: Boolean get() = armedWhat != Prefs.FLIGHT_NONE
    val recovering: Boolean get() = state != State.IDLE

    /**
     * Arm/disarm from whatever knows the load's state (a status packet saying
     * loadOn, a test starting, the user pressing LOAD ON). Cheap to call
     * repeatedly: the pref only writes when the value actually changes.
     */
    fun arm(what: Int) {
        armedWhat = what
        Prefs.armInFlight(app, what)
    }

    fun disarm() {
        if (!armed && state == State.IDLE) return
        armedWhat = Prefs.FLIGHT_NONE
        Prefs.clearInFlight(app)
        if (state != State.IDLE) cancelAttempt()
        state = State.IDLE
    }

    /**
     * The user has taken manual control (started a scan, or a manual connect).
     * Abandon any recovery and disarm: they will reconnect and manage the load
     * themselves, and the guard must not fight that by stopping their scan
     * underneath them. Safety is not lost — a genuine unattended drop still
     * arms and recovers, and the crash flag still covers a process death.
     */
    fun standDown() {
        cancelAttempt()
        state = State.IDLE
        armedWhat = Prefs.FLIGHT_NONE
        wasConnected = false
        Prefs.clearInFlight(app)
    }

    /**
     * Called on every BLE state change. Recovery fires ONLY on a real drop from a
     * live link (CONNECTED -> not), never on the user's own scan/connect
     * transitions from idle — otherwise starting a scan while armed would trigger
     * a reconnect loop that stop-scans on top of the user.
     */
    fun onConnState(st: El15BleManager.State) {
        if (st == El15BleManager.State.CONNECTED) {
            wasConnected = true
            // A reconnect that lands mid-recovery is the recovery succeeding.
            if (state == State.RECOVERING) succeed()
            return
        }
        val dropped = wasConnected
        wasConnected = false
        if (!dropped || !armed || state != State.IDLE) return
        start(
            if (armedWhat == Prefs.FLIGHT_CAPACITY) "Link lost during a discharge"
            else "Link lost with the load ON"
        )
    }

    /**
     * Also the entry point for launch-time recovery, where there was no live link
     * to lose — the previous session simply never got to turn the load off.
     */
    fun start(why: String) {
        if (state == State.RECOVERING) return
        if (!haveTarget()) {   // nothing to reconnect to; say so rather than pretend
            listener?.onGuardAlert(
                "LOAD MAY STILL BE ON",
                "No known device to reconnect to - disconnect the load manually.",
                false,
            )
            return
        }
        state = State.RECOVERING
        attempts = 0
        listener?.onGuardAlert("LOAD MAY STILL BE ON", why, false)
        listener?.onGuardAlarm()
        tryOnce()
    }

    /** Retry a failed recovery (from a UI button). */
    fun retry() {
        if (state == State.FAILED) state = State.IDLE
        start("Retrying")
    }

    /**
     * Recover after a process death that left the flag armed. Returns the flight
     * kind that was recorded, or [Prefs.FLIGHT_NONE] if the last session shut
     * down cleanly.
     */
    fun checkCrashRecovery(): Int {
        val what = Prefs.inFlight(app)
        if (what == Prefs.FLIGHT_NONE) return Prefs.FLIGHT_NONE
        armedWhat = what
        start(
            when (what) {
                Prefs.FLIGHT_CAPACITY -> "The app closed during a battery discharge"
                Prefs.FLIGHT_RTEST -> "The app closed during a resistance sweep"
                else -> "The app closed with the load ON"
            }
        )
        return what
    }

    // ---- Attempt loop --------------------------------------------------------
    private fun tryOnce() {
        if (state != State.RECOVERING) return
        attempts++
        listener?.onGuardAlert(
            "LOAD MAY STILL BE ON",
            "Reconnecting to force LOAD OFF - attempt $attempts of $MAX_ATTEMPTS...",
            false,
        )

        // Already up (the link came back on its own): just kill the load.
        if (ble.state == El15BleManager.State.CONNECTED) { succeed(); return }

        val target = targetAddress()
        if (target == null || !ble.reconnect(target)) {
            scheduleNextAttempt()
            return
        }
        // Give this attempt a deadline. El15BleManager has its own scan timeout,
        // but the guard must keep its own clock so a stack that never calls back
        // still advances the loop.
        val deadline = Runnable { if (state == State.RECOVERING) scheduleNextAttempt() }
        attemptDeadline = deadline
        main.postDelayed(deadline, ATTEMPT_TIMEOUT_MS)
    }

    private fun scheduleNextAttempt() {
        cancelAttempt()
        if (state != State.RECOVERING) return
        if (attempts >= MAX_ATTEMPTS) {
            state = State.FAILED
            val m = "The load did not answer. DISCONNECT IT BY HAND - it may still " +
                "be drawing current. Tap to retry."
            listener?.onGuardFailed(m)
            listener?.onGuardAlarm()
            return
        }
        listener?.onGuardAlarm()
        val next = Runnable { tryOnce() }
        attemptDeadline = next
        main.postDelayed(next, RETRY_INTERVAL_MS)
    }

    private fun succeed() {
        cancelAttempt()
        forceOff()
        state = State.IDLE
        armedWhat = Prefs.FLIGHT_NONE
        Prefs.clearInFlight(app)
        listener?.onGuardAlert(
            "LOAD FORCED OFF",
            "Reconnected and shut the load down. Check the setup before restarting.",
            true,
        )
    }

    private fun cancelAttempt() {
        attemptDeadline?.let { main.removeCallbacks(it) }
        attemptDeadline = null
    }

    /**
     * Repeat the shutdown: a single write can be lost on a link that has only
     * just come back, and this is the one command that must land. Spaced out on
     * the main thread rather than with a blocking sleep, since the GATT queue
     * needs to turn over between writes.
     */
    private fun forceOff() {
        ble.emergencyOff()
        for (k in 1..3) {
            main.postDelayed({
                ble.setSetpoint(0f)
                ble.setLoad(false)
            }, k * FORCE_OFF_SPACING_MS)
        }
    }

    /** Prefer this session's peer; fall back to the stored one after a relaunch. */
    private fun haveTarget(): Boolean = targetAddress() != null

    private fun targetAddress(): String? =
        ble.lastAddress ?: Prefs.lastDevice(app)?.first

    companion object {
        private const val MAX_ATTEMPTS = 8
        private const val RETRY_INTERVAL_MS = 2500L
        private const val ATTEMPT_TIMEOUT_MS = 6000L
        private const val FORCE_OFF_SPACING_MS = 120L
    }
}
