package com.loadtester.el15

import android.content.Context
import androidx.appcompat.app.AppCompatDelegate
import androidx.preference.PreferenceManager

/** Centralised access to user settings (backed by default SharedPreferences). */
object Prefs {
    const val THEME_MODE = "pref_theme_mode"     // system | light | dark
    const val POLL_MS = "pref_poll_ms"
    const val SAFETY_PCT = "pref_safety_pct"
    const val KEEP_SCREEN_ON = "pref_keep_screen_on"
    const val ALARM_LOW_V = "pref_alarm_low_v"
    const val ALARM_HIGH_T = "pref_alarm_high_t"
    const val SNAPSHOT_MIN = "pref_snapshot_min"
    const val LAST_DEV_ADDR = "pref_last_dev_addr"
    const val LAST_DEV_NAME = "pref_last_dev_name"
    const val AUTO_CONNECT = "pref_auto_connect"

    // ---- Demo circuit --------------------------------------------------------
    // The firmware deliberately has no on-device simulator (it was bench-tested
    // against a separate Android simulator app over a real BLE link). The phone
    // keeps an in-process one anyway: without it there is no way to see the app
    // work before an EL15 is wired up. It is a development aid, not a feature —
    // nothing it produces is a measurement.
    const val DEMO_EMF = "pref_demo_emf"
    const val DEMO_R = "pref_demo_r"

    // ---- Probe wiring --------------------------------------------------------
    const val FOUR_WIRE = "pref_four_wire"
    const val TARE_OHM = "pref_tare_ohm"

    // ---- R-Test sweep --------------------------------------------------------
    const val SWEEP_S = "pref_sweep_s"
    const val SWEEP_START_A = "pref_sweep_start_a"
    const val SWEEP_MAX_A = "pref_sweep_max_a"
    const val SETTLE_MS = "pref_settle_ms"
    const val FUSE_A = "pref_fuse_a"

    // ---- Battery capacity test ----------------------------------------------
    const val BATT_CHEM = "pref_batt_chem"
    const val BATT_CELLS = "pref_batt_cells"
    const val BATT_CUTOFF_V = "pref_batt_cutoff_v"
    const val BATT_CURRENT_A = "pref_batt_current_a"
    const val BATT_RATED_AH = "pref_batt_rated_ah"
    const val BATT_REST_S = "pref_batt_rest_s"

    // ---- Crash / link recovery ----------------------------------------------
    /**
     * What the load was doing when we last knew. Mirrors the firmware's
     * `prefs::armInFlight` NVS flag: written SYNCHRONOUSLY so a process death
     * cannot lose it, and read at next launch to offer a reconnect-and-kill.
     */
    const val IN_FLIGHT = "pref_in_flight"

    const val FLIGHT_NONE = 0
    const val FLIGHT_MANUAL = 1
    const val FLIGHT_RTEST = 2
    const val FLIGHT_CAPACITY = 3

    private fun p(ctx: Context) = PreferenceManager.getDefaultSharedPreferences(ctx)

    private fun str(ctx: Context, key: String, def: String) = p(ctx).getString(key, def) ?: def

    private fun f(ctx: Context, key: String, def: Float, lo: Float, hi: Float): Float =
        str(ctx, key, def.toString()).toFloatOrNull()?.coerceIn(lo, hi) ?: def

    fun themeMode(ctx: Context): String = str(ctx, THEME_MODE, "system")

    fun applyNightMode(ctx: Context) {
        AppCompatDelegate.setDefaultNightMode(
            when (themeMode(ctx)) {
                "light" -> AppCompatDelegate.MODE_NIGHT_NO
                "dark" -> AppCompatDelegate.MODE_NIGHT_YES
                else -> AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM
            }
        )
    }

    /**
     * Status poll interval. The floor is 20 ms rather than the old 100 ms: the
     * R-Test sweep fits EVERY packet that arrives, so the sample rate directly
     * tightens the resistance uncertainty, and the firmware's own default is
     * 50 ms (20 Hz). Whether the phone's radio sustains that depends on the
     * negotiated connection interval — see El15BleManager's high-priority
     * request — but nothing here should stop it trying.
     */
    fun pollMs(ctx: Context): Long =
        str(ctx, POLL_MS, "50").toLongOrNull()?.coerceIn(20, 5000) ?: 50L

    fun safetyFactor(ctx: Context): Float =
        (str(ctx, SAFETY_PCT, "80").toFloatOrNull()?.coerceIn(10f, 95f) ?: 80f) / 100f

    fun demoEmf(ctx: Context): Float = f(ctx, DEMO_EMF, 12.6f, 0.1f, 100f)
    fun demoR(ctx: Context): Float = f(ctx, DEMO_R, 0.35f, 0f, 100f)

    fun setDemo(ctx: Context, emf: Float, r: Float) {
        p(ctx).edit().putString(DEMO_EMF, emf.toString()).putString(DEMO_R, r.toString()).apply()
    }

    fun keepScreenOn(ctx: Context): Boolean = p(ctx).getBoolean(KEEP_SCREEN_ON, true)

    fun autoConnect(ctx: Context): Boolean = p(ctx).getBoolean(AUTO_CONNECT, false)

    /** 0 disables the alarm. */
    fun alarmLowV(ctx: Context): Float = f(ctx, ALARM_LOW_V, 0f, 0f, 60f)
    fun alarmHighTemp(ctx: Context): Float = f(ctx, ALARM_HIGH_T, 0f, 0f, 200f)

    /** Periodic CSV snapshot interval in minutes; 0 = off. */
    fun snapshotMinutes(ctx: Context): Int =
        str(ctx, SNAPSHOT_MIN, "0").toIntOrNull()?.coerceIn(0, 1440) ?: 0

    // ---- Probe wiring --------------------------------------------------------
    /**
     * 4-wire (Kelvin) probing. The load senses voltage at its own terminals, so a
     * 2-wire hook-up reads short by the drop across the leads; give it the lead
     * resistance ([tareOhm], typed or measured by an R-Test tare sweep) and every
     * reading is corrected back to the part. 4-wire applies no correction.
     */
    fun fourWire(ctx: Context): Boolean = p(ctx).getBoolean(FOUR_WIRE, false)

    fun tareOhm(ctx: Context): Float = f(ctx, TARE_OHM, 0f, 0f, 10f)

    fun setTareOhm(ctx: Context, ohm: Float) {
        p(ctx).edit().putString(TARE_OHM, "%.6f".format(ohm)).apply()
    }

    fun setFourWire(ctx: Context, on: Boolean) {
        p(ctx).edit().putBoolean(FOUR_WIRE, on).apply()
    }

    // ---- R-Test sweep --------------------------------------------------------
    fun sweepSeconds(ctx: Context): Long =
        str(ctx, SWEEP_S, "30").toLongOrNull()
            ?.coerceIn(ResistanceTest.MIN_SWEEP_S, ResistanceTest.MAX_SWEEP_S) ?: 30L

    fun sweepStartA(ctx: Context): Float = f(ctx, SWEEP_START_A, 0f, 0f, 40f)

    /** 0 = use the whole safe headroom the fuse rating allows. */
    fun sweepMaxA(ctx: Context): Float = f(ctx, SWEEP_MAX_A, 0f, 0f, 40f)

    fun settleMs(ctx: Context): Long =
        str(ctx, SETTLE_MS, "0").toLongOrNull()?.coerceIn(0, 10_000) ?: 0L

    fun fuseA(ctx: Context): Float = f(ctx, FUSE_A, 5f, 0.1f, 100f)

    fun setFuseA(ctx: Context, a: Float) {
        p(ctx).edit().putString(FUSE_A, a.toString()).apply()
    }

    // ---- Battery capacity test ----------------------------------------------
    fun battChem(ctx: Context): Int =
        str(ctx, BATT_CHEM, "0").toIntOrNull()?.coerceIn(0, BatteryModel.CHEM_N - 1) ?: 0

    fun battCells(ctx: Context): Int =
        str(ctx, BATT_CELLS, "1").toIntOrNull()?.coerceIn(0, 40) ?: 1

    fun battCutoffV(ctx: Context): Float = f(ctx, BATT_CUTOFF_V, 3.0f, 0.1f, 60f)
    fun battCurrentA(ctx: Context): Float = f(ctx, BATT_CURRENT_A, 1.0f, 0.01f, 12f)

    /** Optional nameplate capacity; 0 = unknown. */
    fun battRatedAh(ctx: Context): Float = f(ctx, BATT_RATED_AH, 0f, 0f, 2000f)

    /** Post-cutoff rebound window in seconds; 0 = none. */
    fun battRestS(ctx: Context): Long =
        str(ctx, BATT_REST_S, "60").toLongOrNull()?.coerceIn(0, 3600) ?: 60L

    fun setBattSetup(
        ctx: Context, chem: Int, cells: Int, cutoffV: Float,
        currentA: Float, ratedAh: Float,
    ) {
        p(ctx).edit()
            .putString(BATT_CHEM, chem.toString())
            .putString(BATT_CELLS, cells.toString())
            .putString(BATT_CUTOFF_V, cutoffV.toString())
            .putString(BATT_CURRENT_A, currentA.toString())
            .putString(BATT_RATED_AH, ratedAh.toString())
            .apply()
    }

    fun setSweepSetup(ctx: Context, sweepS: Long, startA: Float, maxA: Float) {
        p(ctx).edit()
            .putString(SWEEP_S, sweepS.toString())
            .putString(SWEEP_START_A, startA.toString())
            .putString(SWEEP_MAX_A, maxA.toString())
            .apply()
    }

    // ---- Device ---------------------------------------------------------------
    fun lastDevice(ctx: Context): Pair<String, String>? {
        val a = p(ctx).getString(LAST_DEV_ADDR, null) ?: return null
        return a to (p(ctx).getString(LAST_DEV_NAME, null) ?: "EL15")
    }

    fun setLastDevice(ctx: Context, addr: String, name: String) {
        p(ctx).edit().putString(LAST_DEV_ADDR, addr).putString(LAST_DEV_NAME, name).apply()
    }

    // ---- Crash / link recovery ------------------------------------------------
    /**
     * Record that the load is (or might be) energised.
     *
     * Written with `commit()`, not `apply()`: `apply()` returns immediately and
     * flushes on a background thread, so a process kill between the two loses
     * exactly the flag whose entire purpose is surviving one. The firmware makes
     * the same trade with a synchronous NVS write. Cheap to call repeatedly —
     * this no-ops when the value has not changed.
     */
    fun armInFlight(ctx: Context, what: Int) {
        if (inFlight(ctx) == what) return
        @Suppress("ApplySharedPref")
        p(ctx).edit().putInt(IN_FLIGHT, what).commit()
    }

    fun clearInFlight(ctx: Context) = armInFlight(ctx, FLIGHT_NONE)

    fun inFlight(ctx: Context): Int = p(ctx).getInt(IN_FLIGHT, FLIGHT_NONE)
}
