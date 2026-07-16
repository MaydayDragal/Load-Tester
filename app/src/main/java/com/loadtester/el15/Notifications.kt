package com.loadtester.el15

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat

/** Notification channels + helpers (monitor foreground, alarms, completions). */
object Notifications {

    const val CH_MONITOR = "monitor"
    const val CH_ALERTS = "alerts"
    const val ID_MONITOR = 1
    private const val ID_ALERT = 2
    private const val ID_DONE = 3

    fun ensureChannels(ctx: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(
            NotificationChannel(CH_MONITOR, "Live monitor",
                NotificationManager.IMPORTANCE_LOW).apply {
                description = "Ongoing readout while connected to the load"
            })
        nm.createNotificationChannel(
            NotificationChannel(CH_ALERTS, "Alarms & test results",
                NotificationManager.IMPORTANCE_HIGH).apply {
                description = "Threshold alarms and test completions"
            })
    }

    fun canPost(ctx: Context): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.POST_NOTIFICATIONS) ==
            PackageManager.PERMISSION_GRANTED

    private fun openAppIntent(ctx: Context): PendingIntent =
        PendingIntent.getActivity(ctx, 0,
            Intent(ctx, MainActivity::class.java).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)

    fun monitor(ctx: Context, text: String, subText: String): Notification {
        ensureChannels(ctx)
        val loadOff = PendingIntent.getService(ctx, 1,
            Intent(ctx, MonitorService::class.java).setAction(MonitorService.ACTION_LOAD_OFF),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val disconnect = PendingIntent.getService(ctx, 2,
            Intent(ctx, MonitorService::class.java).setAction(MonitorService.ACTION_DISCONNECT),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        return NotificationCompat.Builder(ctx, CH_MONITOR)
            .setSmallIcon(R.drawable.ic_zap)
            .setContentTitle(text)
            .setContentText(subText)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(openAppIntent(ctx))
            .addAction(0, "Load OFF", loadOff)
            .addAction(0, "Disconnect", disconnect)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .build()
    }

    fun alert(ctx: Context, title: String, message: String) {
        if (!canPost(ctx)) return
        ensureChannels(ctx)
        val n = NotificationCompat.Builder(ctx, CH_ALERTS)
            .setSmallIcon(R.drawable.ic_warning)
            .setContentTitle(title)
            .setContentText(message)
            .setStyle(NotificationCompat.BigTextStyle().bigText(message))
            .setAutoCancel(true)
            .setContentIntent(openAppIntent(ctx))
            .build()
        (ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
            .notify(ID_ALERT, n)
    }

    fun testDone(ctx: Context, title: String, message: String, recordId: String, session: Boolean) {
        if (!canPost(ctx)) return
        ensureChannels(ctx)
        val target = if (session) SessionResultActivity::class.java else ResultActivity::class.java
        val open = PendingIntent.getActivity(ctx, recordId.hashCode(),
            Intent(ctx, target).putExtra(ResultActivity.EXTRA_RECORD_ID, recordId),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val n = NotificationCompat.Builder(ctx, CH_ALERTS)
            .setSmallIcon(R.drawable.ic_check_notif)
            .setContentTitle(title)
            .setContentText(message)
            .setAutoCancel(true)
            .setContentIntent(open)
            .build()
        (ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
            .notify(ID_DONE, n)
    }
}
