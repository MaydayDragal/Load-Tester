package com.loadtester.el15

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.widget.RemoteViews
import java.util.Locale

/**
 * Home-screen widget with the live V/I/P readout and an emergency Load-OFF
 * button. [DeviceCore] pushes throttled updates while a status stream is
 * flowing; otherwise the widget shows the last-known connection state.
 */
class LiveWidget : AppWidgetProvider() {

    override fun onUpdate(context: Context, mgr: AppWidgetManager, ids: IntArray) {
        render(context, mgr, ids, DeviceCore.peek()?.lastStatus.takeIf {
            DeviceCore.peek()?.isConnected == true
        })
    }

    companion object {
        private var lastPushMs = 0L

        /** Throttled live update from the status stream (main thread). */
        fun push(ctx: Context, status: El15Status) {
            val now = System.currentTimeMillis()
            if (now - lastPushMs < 2000) return
            lastPushMs = now
            update(ctx, status)
        }

        /** Re-render from current core state (connect/disconnect edges). */
        fun refresh(ctx: Context) {
            lastPushMs = 0L
            val core = DeviceCore.peek()
            update(ctx, if (core?.isConnected == true) core.lastStatus else null)
        }

        private fun update(ctx: Context, status: El15Status?) {
            try {
                val mgr = AppWidgetManager.getInstance(ctx) ?: return
                val ids = mgr.getAppWidgetIds(ComponentName(ctx, LiveWidget::class.java))
                if (ids.isEmpty()) return
                render(ctx, mgr, ids, status)
            } catch (ignored: Exception) {
            }
        }

        private fun render(ctx: Context, mgr: AppWidgetManager, ids: IntArray, status: El15Status?) {
            val core = DeviceCore.peek()
            val connected = core?.isConnected == true
            val views = RemoteViews(ctx.packageName, R.layout.widget_live)

            views.setTextViewText(R.id.widgetState, when {
                !connected -> ctx.getString(R.string.disconnected)
                core?.isDemo == true -> "DEMO"
                else -> "CONNECTED"
            })
            views.setTextViewText(R.id.widgetReadout,
                if (connected && status != null)
                    String.format(Locale.US, "%.2fV · %.3fA · %.1fW",
                        status.voltage, status.current, status.power)
                else "— · — · —")
            views.setTextViewText(R.id.widgetSub, when {
                !connected -> ctx.getString(R.string.widget_tap_to_open)
                core?.busy == true -> core.lastProgressText.take(40)
                status?.loadOn == true -> "Load ON · ${status.modeName}"
                status != null -> "Load off · ${status.modeName}"
                else -> ""
            })

            val open = PendingIntent.getActivity(ctx, 10,
                Intent(ctx, MainActivity::class.java).setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            views.setOnClickPendingIntent(R.id.widgetRoot, open)

            val loadOff = PendingIntent.getService(ctx, 11,
                Intent(ctx, MonitorService::class.java).setAction(MonitorService.ACTION_LOAD_OFF),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            views.setOnClickPendingIntent(R.id.widgetLoadOff, loadOff)
            views.setViewVisibility(R.id.widgetLoadOff,
                if (connected) android.view.View.VISIBLE else android.view.View.GONE)

            for (id in ids) mgr.updateAppWidget(id, views)
        }
    }
}
