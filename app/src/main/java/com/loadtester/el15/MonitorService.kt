package com.loadtester.el15

import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * Foreground wrapper around [DeviceCore]: keeps the process alive while a
 * device is connected or a test is running, and surfaces a live-readout
 * notification with Load-OFF / Disconnect actions. The service owns no state
 * of its own — it starts when a connection appears and stops itself when the
 * session ends.
 */
class MonitorService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val core = DeviceCore.get(this)
        when (intent?.action) {
            ACTION_LOAD_OFF -> core.emergencyLoadOff()
            ACTION_DISCONNECT -> {
                core.disconnect()
                stopSelfSafely()
                return START_NOT_STICKY
            }
        }
        if (!core.isConnected && !core.busy) {
            stopSelfSafely()
            return START_NOT_STICKY
        }
        promote(core.lastStatus)
        return START_NOT_STICKY
    }

    private fun promote(status: El15Status?) {
        val core = DeviceCore.get(this)
        val title = status?.let {
            "%.2f V · %.3f A · %.1f W".format(it.voltage, it.current, it.power)
        } ?: "Connected"
        val sub = when {
            core.session.running || core.tester.running || core.calibrator.running ->
                core.lastProgressText.ifEmpty { "Test running" }
            core.isDemo -> "Demo simulator"
            else -> "EL15"
        }
        val n = Notifications.monitor(this, title, sub)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(Notifications.ID_MONITOR, n,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(Notifications.ID_MONITOR, n)
        }
    }

    private fun stopSelfSafely() {
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    companion object {
        const val ACTION_LOAD_OFF = "com.loadtester.el15.LOAD_OFF"
        const val ACTION_DISCONNECT = "com.loadtester.el15.DISCONNECT"

        private var lastTickMs = 0L

        /** Start (or keep) the foreground session notification. */
        fun ensureRunning(ctx: Context) {
            try {
                ctx.startService(Intent(ctx, MonitorService::class.java))
            } catch (ignored: Exception) {
                // Backgrounded without an exemption: the notification appears on
                // the next foreground entry; the core keeps running regardless.
            }
        }

        /** Re-evaluate running/stopped state after a session change. */
        fun refresh(ctx: Context) = ensureRunning(ctx)

        /** Throttled live-notification update from the status stream. */
        fun tick(ctx: Context, status: El15Status) {
            val now = System.currentTimeMillis()
            if (now - lastTickMs < 1000) return
            lastTickMs = now
            val core = DeviceCore.peek() ?: return
            if (!core.isConnected) return
            if (!Notifications.canPost(ctx)) return
            try {
                val title = "%.2f V · %.3f A · %.1f W"
                    .format(status.voltage, status.current, status.power)
                val sub = if (core.busy) core.lastProgressText.ifEmpty { "Test running" }
                else if (core.isDemo) "Demo simulator" else "EL15"
                val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE)
                    as android.app.NotificationManager
                nm.notify(Notifications.ID_MONITOR, Notifications.monitor(ctx, title, sub))
            } catch (ignored: Exception) {}
        }
    }
}
