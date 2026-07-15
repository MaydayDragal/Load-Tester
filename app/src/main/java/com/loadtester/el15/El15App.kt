package com.loadtester.el15

import android.app.Application

class El15App : Application() {
    override fun onCreate() {
        super.onCreate()
        Prefs.applyNightMode(this)
    }
}
