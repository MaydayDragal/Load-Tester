package com.loadtester.el15

import android.content.SharedPreferences
import android.os.Bundle
import android.text.InputType
import androidx.preference.EditTextPreference
import androidx.preference.PreferenceFragmentCompat
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.appbar.MaterialToolbar

class SettingsActivity : BaseActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)
        findViewById<MaterialToolbar>(R.id.toolbar).setNavigationOnClickListener { finish() }
        if (savedInstanceState == null) {
            supportFragmentManager.beginTransaction()
                .replace(R.id.settingsContainer, SettingsFragment())
                .commit()
        }
    }

    class SettingsFragment : PreferenceFragmentCompat(),
        SharedPreferences.OnSharedPreferenceChangeListener {

        override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
            setPreferencesFromResource(R.xml.preferences, rootKey)

            findPreference<androidx.preference.Preference>("pref_about")?.apply {
                summary = "Version ${BuildConfig.VERSION_NAME} · tap for details"
                setOnPreferenceClickListener { showAbout(); true }
            }

            // Numeric keyboards for the numeric preferences.
            val integer = InputType.TYPE_CLASS_NUMBER
            val decimal = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
            mapOf(
                Prefs.POLL_MS to integer,
                Prefs.SAFETY_PCT to integer,
                Prefs.STEPS to integer,
                Prefs.SETTLE_MS to integer,
                Prefs.SAMPLE_MS to integer,
                Prefs.DEMO_EMF to decimal,
                Prefs.DEMO_R to decimal,
                Prefs.ALARM_LOW_V to decimal,
                Prefs.ALARM_HIGH_T to decimal,
                Prefs.SNAPSHOT_MIN to integer,
                Prefs.RETENTION to integer,
            ).forEach { (key, type) ->
                findPreference<EditTextPreference>(key)?.setOnBindEditTextListener { it.inputType = type }
            }
        }

        override fun onResume() {
            super.onResume()
            preferenceManager.sharedPreferences?.registerOnSharedPreferenceChangeListener(this)
        }

        override fun onPause() {
            super.onPause()
            preferenceManager.sharedPreferences?.unregisterOnSharedPreferenceChangeListener(this)
        }

        override fun onSharedPreferenceChanged(sp: SharedPreferences?, key: String?) {
            if (key == Prefs.THEME_MODE) {
                Prefs.applyNightMode(requireContext())
                activity?.recreate()
            }
        }

        private fun showAbout() {
            MaterialAlertDialogBuilder(requireContext())
                .setTitle(getString(R.string.app_name))
                .setMessage(getString(R.string.about_body, BuildConfig.VERSION_NAME))
                .setPositiveButton(android.R.string.ok, null)
                .show()
        }
    }
}
