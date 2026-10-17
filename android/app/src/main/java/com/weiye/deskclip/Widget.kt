package com.weiye.deskclip

/* Home-screen widget with one 同步 button: sends the phone's current
 * clipboard text to the board. Android 10+ only lets the focused app read the
 * clipboard, so the button opens ClipSyncActivity (transparent, no animation),
 * which reads it once it has window focus, hands it to SyncService (same send
 * path as the main screen's 同步) and finishes immediately. */

import android.Manifest
import android.app.Activity
import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ClipboardManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.RemoteViews
import android.widget.Toast

class SyncWidget : AppWidgetProvider() {
    override fun onUpdate(context: Context, manager: AppWidgetManager, ids: IntArray) {
        update(context, null)
    }

    companion object {
        /** Redraw every instance; status null keeps the default label. */
        fun update(context: Context, status: String?) {
            val manager = AppWidgetManager.getInstance(context)
            val ids = manager.getAppWidgetIds(ComponentName(context, SyncWidget::class.java))
            if (ids.isEmpty()) return
            val intent = Intent(context, ClipSyncActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION)
            val tap = PendingIntent.getActivity(context, 0, intent,
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            val views = RemoteViews(context.packageName, R.layout.widget_sync)
            views.setOnClickPendingIntent(R.id.widget_sync, tap)
            views.setTextViewText(R.id.widget_status, status ?: "DeskClipboard")
            manager.updateAppWidget(ids, views)
        }
    }
}

class ClipSyncActivity : Activity() {
    private var done = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        @Suppress("DEPRECATION")
        overridePendingTransition(0, 0)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || done) return
        done = true
        report(sync())
        finish()
        @Suppress("DEPRECATION")
        overridePendingTransition(0, 0)
    }

    private fun sync(): String {
        if (Prefs.load(this) == null) return "未配对，请先打开 App 配对"
        val needed = if (Build.VERSION.SDK_INT >= 31) listOf(Manifest.permission.BLUETOOTH_CONNECT)
        else listOf(Manifest.permission.ACCESS_FINE_LOCATION)
        if (needed.any { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }) return "缺少蓝牙权限，请打开 App 授权"
        val clip = getSystemService(ClipboardManager::class.java).primaryClip
        val text = if (clip != null && clip.itemCount > 0) clip.getItemAt(0).coerceToText(this)?.toString().orEmpty() else ""
        if (text.isEmpty()) return "剪贴板为空"
        if (text.toByteArray().size > MAX_TEXT) return errorMessage("text_too_large_64KiB")
        // Result toast (已同步 / error) comes from SyncService when the board answers.
        SyncService.start(this, Intent(SyncService.ACTION_SEND).putExtra(SyncService.EXTRA_TEXT, text)
            .putExtra(SyncService.EXTRA_FROM_WIDGET, true))
        return if (SyncState.connected) "正在同步…" else "未连接，连接后自动同步"
    }

    private fun report(message: String) {
        Toast.makeText(applicationContext, message, Toast.LENGTH_SHORT).show()
        SyncWidget.update(applicationContext, message)
    }
}
