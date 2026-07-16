package com.loadtester.el15

import android.app.PendingIntent
import android.content.Intent
import android.graphics.drawable.Icon
import android.os.Build
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService

/**
 * Quick Settings tile: shows the connection/load state at a glance. Tapping it
 * while the load is sinking commands an emergency Load OFF; otherwise it opens
 * the app. (Deliberately never turns the load ON — an accidental tap in the
 * shade must not start sinking current.)
 */
class LoadTileService : TileService() {

    override fun onStartListening() {
        super.onStartListening()
        renderTile()
    }

    override fun onClick() {
        super.onClick()
        val core = DeviceCore.peek()
        if (core?.isConnected == true && core.lastStatus?.loadOn == true) {
            core.emergencyLoadOff()
            renderTile()
        } else {
            openApp()
        }
    }

    @Suppress("DEPRECATION", "StartActivityAndCollapseDeprecated")
    private fun openApp() {
        val intent = Intent(this, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startActivityAndCollapse(PendingIntent.getActivity(this, 12, intent,
                    PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE))
            } else {
                startActivityAndCollapse(intent)
            }
        } catch (ignored: Exception) {
        }
    }

    private fun renderTile() {
        val tile = qsTile ?: return
        val core = DeviceCore.peek()
        val status = core?.lastStatus
        when {
            core?.isConnected == true && status?.loadOn == true -> {
                tile.state = Tile.STATE_ACTIVE
                tile.label = getString(R.string.tile_load_off)
                tile.icon = Icon.createWithResource(this, R.drawable.ic_zap)
                if (Build.VERSION.SDK_INT >= 29) {
                    tile.subtitle = "%.2f V · %.3f A".format(status.voltage, status.current)
                }
            }
            core?.isConnected == true -> {
                tile.state = Tile.STATE_INACTIVE
                tile.label = getString(R.string.app_name)
                tile.icon = Icon.createWithResource(this, R.drawable.ic_zap)
                if (Build.VERSION.SDK_INT >= 29) tile.subtitle = "Connected · load off"
            }
            else -> {
                tile.state = Tile.STATE_INACTIVE
                tile.label = getString(R.string.app_name)
                tile.icon = Icon.createWithResource(this, R.drawable.ic_zap)
                if (Build.VERSION.SDK_INT >= 29) tile.subtitle = getString(R.string.disconnected)
            }
        }
        tile.updateTile()
    }
}
