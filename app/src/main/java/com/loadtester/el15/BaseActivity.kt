package com.loadtester.el15

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity

/** Applies the user-selected accent overlay and re-themes when it changes. */
open class BaseActivity : AppCompatActivity() {
    private var appliedAccent = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        appliedAccent = Prefs.accentStyle(this)
        theme.applyStyle(appliedAccent, true)
        super.onCreate(savedInstanceState)
    }

    override fun onResume() {
        super.onResume()
        if (Prefs.accentStyle(this) != appliedAccent) recreate()
    }
}
