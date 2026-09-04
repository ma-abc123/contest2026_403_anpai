package com.aiwatch.companion.ble

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import com.aiwatch.companion.MainActivity
import com.aiwatch.companion.R
import com.aiwatch.companion.WatchApp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * 前台服务：仅负责保活进程 + 展示连接状态通知，BLE 逻辑全部在 WatchBleManager。
 */
class WatchForegroundService : Service() {

    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    override fun onCreate() {
        super.onCreate()
        createChannel()
        startInForeground(notifyText(WatchPhase.IDLE))
        // 跟随连接状态更新通知文案；用户断开（IDLE）时自我停止
        serviceScope.launch {
            (applicationContext as WatchApp).ble.state.collect { st ->
                if (st.phase == WatchPhase.IDLE) {
                    stopSelf()
                } else {
                    notifyManager().notify(NOTIF_ID, buildNotification(notifyText(st.phase, st.deviceName, st.retryAttempt)))
                }
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        serviceScope.cancel()
        super.onDestroy()
    }

    // ------------------------------------------------------------------

    private fun notifyText(phase: WatchPhase, device: String? = null, attempt: Int = 0): String =
        when (phase) {
            WatchPhase.CONNECTED -> "已连接 · ${device ?: "AI-Watch-403"}"
            WatchPhase.RECONNECTING -> "重连中（第 $attempt 次）"
            WatchPhase.SCANNING -> "正在扫描手表…"
            WatchPhase.CONNECTING, WatchPhase.INITIALIZING -> "正在连接手表…"
            else -> "等待手表连接"
        }

    private fun startInForeground(text: String) {
        val n = buildNotification(text)
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    private fun buildNotification(text: String): Notification {
        val contentIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_watch)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(contentIntent)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .build()
    }

    private fun notifyManager(): NotificationManager =
        getSystemService(NotificationManager::class.java)

    private fun createChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.notif_channel_name),
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = getString(R.string.notif_channel_desc)
            setShowBadge(false)
        }
        notifyManager().createNotificationChannel(channel)
    }

    companion object {
        const val CHANNEL_ID = "watch_connection"
        const val NOTIF_ID = 1001
        const val ACTION_STOP = "com.aiwatch.companion.action.STOP_WATCH_SERVICE"

        fun stopIntent(context: Context): Intent =
            Intent(context, WatchForegroundService::class.java).setAction(ACTION_STOP)
    }
}

/** Phase 的别名导入用（避免 UI 依赖 ble 包内部命名混淆） */
typealias WatchPhase = Phase
