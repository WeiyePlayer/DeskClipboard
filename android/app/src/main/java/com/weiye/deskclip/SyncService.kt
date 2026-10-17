package com.weiye.deskclip

/* Foreground service that owns the BLE session so the board can reach the
 * phone while the app is in the background. Items from the board go to the
 * clipboard (plus a toast); nothing is pasted. */

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.widget.Toast
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Shared state for the activity (main thread only). */
object SyncState {
    var status = "未连接"
    var connected = false
    var message = ""
    val listeners = mutableSetOf<() -> Unit>()
    fun changed() = listeners.forEach { it() }
}

class SyncService : Service() {
    private val main = Handler(Looper.getMainLooper())
    private var session: Session? = null
    private var widgetPending = 0 // widget sends awaiting a result toast

    companion object {
        const val ACTION_SEND = "com.weiye.deskclip.SEND"
        const val ACTION_STOP = "com.weiye.deskclip.STOP"
        const val EXTRA_TEXT = "text"
        const val EXTRA_FROM_WIDGET = "from_widget"
        private const val CHANNEL = "sync"
        private const val NOTIFICATION = 1

        fun start(context: Context, intent: Intent = Intent(context, SyncService::class.java)) {
            context.startForegroundService(intent.setClass(context, SyncService::class.java))
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel(CHANNEL, "蓝牙连接", NotificationManager.IMPORTANCE_LOW))
    }

    private fun notification(text: String): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("DeskClipboard")
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
    }

    private fun update(text: String) {
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION, notification(text))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val first = notification(SyncState.status)
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIFICATION, first, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(NOTIFICATION, first)
        }
        if (intent?.action == ACTION_STOP) {
            session?.shutdown()
            session = null
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }
        val creds = Prefs.load(this)
        if (creds == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (session?.isAlive != true) {
            session = Session(creds, { BleLink.open(this, creds.address) }, CardRenderer::render, events).also { it.start() }
        }
        if (intent?.action == ACTION_SEND) {
            val text = intent.getStringExtra(EXTRA_TEXT).orEmpty()
            val fromWidget = intent.getBooleanExtra(EXTRA_FROM_WIDGET, false)
            if (fromWidget) post { widgetPending++ }
            if (session?.send(text) != true) post { SyncState.message = "待发送内容过多，请稍后"; SyncState.changed() }
            else post { SyncState.message = if (SyncState.connected) "正在同步…" else "已排队，连接后同步"; SyncState.changed() }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        session?.shutdown()
        session = null
        super.onDestroy()
    }

    /** Main thread: finish one widget-initiated send with a toast + widget label. */
    private fun widgetResult(message: String) {
        if (widgetPending == 0) return
        widgetPending--
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
        SyncWidget.update(this, message)
    }

    private fun post(block: () -> Unit) {
        main.post(block)
    }

    private val events = object : SessionEvents {
        override fun state(text: String, connected: Boolean) = post {
            SyncState.status = text
            SyncState.connected = connected
            SyncState.changed()
            update(text)
            if (!connected) SyncWidget.update(this@SyncService, "未连接")
            else SyncWidget.update(this@SyncService, null)
        }

        override fun sent(recordId: Long) = post {
            SyncState.message = "已同步到开发板 #$recordId"
            SyncState.changed()
            widgetResult("已同步")
        }

        override fun failed(message: String) = post {
            SyncState.message = message
            SyncState.changed()
            widgetResult(message)
        }

        override fun needsPairing(message: String) = post {
            SyncState.message = message
            SyncState.status = "需要重新配对"
            SyncState.connected = false
            SyncState.changed()
            update("需要重新配对")
        }

        override fun deliver(text: String, deadlineMs: Long): Boolean {
            val done = CountDownLatch(1)
            var ok = false
            main.post {
                try {
                    if (SystemClock.uptimeMillis() < deadlineMs) {
                        val clipboard = getSystemService(ClipboardManager::class.java)
                        clipboard.setPrimaryClip(ClipData.newPlainText("DeskClipboard", text))
                        ok = true
                    }
                } catch (_: Exception) {
                }
                done.countDown()
                if (ok) {
                    Prefs.setLastReceived(this@SyncService, text)
                    SyncState.message = "已收到并复制到剪贴板"
                    SyncState.changed()
                    Toast.makeText(this@SyncService, "DeskClipboard：已复制到剪贴板", Toast.LENGTH_SHORT).show()
                }
            }
            done.await(maxOf(0, deadlineMs - SystemClock.uptimeMillis()), TimeUnit.MILLISECONDS)
            return ok
        }
    }
}
