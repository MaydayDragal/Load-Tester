package com.loadtester.el15

import android.content.Context
import androidx.appcompat.app.AppCompatDelegate
import androidx.preference.PreferenceManager

/** Centralised access to user settings (backed by default SharedPreferences). */
object Prefs {
    const val THEME_MODE = "pref_theme_mode"     // system | light | dark
    const val POLL_MS = "pref_poll_ms"
    const val SAFETY_PCT = "pref_safety_pct"
    const val STEPS = "pref_steps"
    const val SETTLE_MS = "pref_settle_ms"
    const val SAMPLE_MS = "pref_sample_ms"
    const val DEMO_EMF = "pref_demo_emf"
    const val DEMO_R = "pref_demo_r"
    const val KEEP_SCREEN_ON = "pref_keep_screen_on"

    private fun p(ctx: Context) = PreferenceManager.getDefaultSharedPreferences(ctx)

    private fun str(ctx: Context, key: String, def: String) = p(ctx).getString(key, def) ?: def

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

    fun pollMs(ctx: Context): Long = str(ctx, POLL_MS, "500").toLongOrNull()?.coerceIn(100, 5000) ?: 500L

    fun safetyFactor(ctx: Context): Float =
        (str(ctx, SAFETY_PCT, "80").toFloatOrNull()?.coerceIn(10f, 95f) ?: 80f) / 100f

    fun steps(ctx: Context): Int = str(ctx, STEPS, "8").toIntOrNull()?.coerceIn(2, 1000) ?: 8
    fun settleMs(ctx: Context): Long = str(ctx, SETTLE_MS, "800").toLongOrNull()?.coerceAtLeast(0) ?: 800L
    fun sampleMs(ctx: Context): Long = str(ctx, SAMPLE_MS, "1500").toLongOrNull()?.coerceAtLeast(0) ?: 1500L

    fun demoEmf(ctx: Context): Float = str(ctx, DEMO_EMF, "12.6").toFloatOrNull()?.coerceIn(0.1f, 100f) ?: 12.6f
    fun demoR(ctx: Context): Float = str(ctx, DEMO_R, "0.35").toFloatOrNull()?.coerceIn(0f, 100f) ?: 0.35f

    fun setDemo(ctx: Context, emf: Float, r: Float) {
        p(ctx).edit().putString(DEMO_EMF, emf.toString()).putString(DEMO_R, r.toString()).apply()
    }

    fun keepScreenOn(ctx: Context): Boolean = p(ctx).getBoolean(KEEP_SCREEN_ON, true)
}
