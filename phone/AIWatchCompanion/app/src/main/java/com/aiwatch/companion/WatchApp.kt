package com.aiwatch.companion

import android.Manifest
import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Intent
import android.content.pm.PackageManager
import android.media.RingtoneManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.aiwatch.companion.ai.AiConfigRepository
import com.aiwatch.companion.ai.AiSessionManager
import com.aiwatch.companion.ble.BleProto
import com.aiwatch.companion.ble.WatchBleManager
import com.aiwatch.companion.ble.WatchLog
import com.aiwatch.companion.data.FallRecord
import com.aiwatch.companion.data.FallRepository
import com.aiwatch.companion.data.MotionRecorder
import com.aiwatch.companion.data.ReminderRepository
import com.aiwatch.companion.data.SettingsRepository
import kotlinx.coroutines.flow.MutableStateFlow
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 应用入口：持有应用级单例（连接管理器 / 日志 / 数据仓库 / AI 会话管理 / 运动数据）。
 */
class WatchApp : Application() {

    lateinit var log: WatchLog
        private set
    lateinit var ble: WatchBleManager
        private set
    lateinit var reminders: ReminderRepository
        private set
    lateinit var settings: SettingsRepository
        private set
    lateinit var aiConfig: AiConfigRepository
        private set
    lateinit var ai: AiSessionManager
        private set
    lateinit var falls: FallRepository
        private set
    lateinit var motion: MotionRecorder
        private set

    /** 前台跌倒告警弹窗数据（event=1 或通知点击时设置，UI 消费后置 null） */
    val fallAlert = MutableStateFlow<FallRecord?>(null)

    override fun onCreate() {
        super.onCreate()
        log = WatchLog()
        reminders = ReminderRepository(this)
        settings = SettingsRepository(this)
        ble = WatchBleManager(this, log)
        aiConfig = AiConfigRepository(this)
        ai = AiSessionManager(this, ble, reminders, aiConfig, log)
        falls = FallRepository(this)
        motion = MotionRecorder(this)
        // v2：手表 AI 触发（f3 sensor_type=0x10）→ 会话管理器
        ble.aiTriggerListener = { source, hr, spo2 ->
            ai.onTrigger(source, hr, spo2)
        }
        // v3：疑似跌倒（0x11）→ 入库 + 通知 + 前台置顶弹窗（不允许丢）
        ble.fallListener = { f ->
            val rec = FallRecord(f.event, f.impactMg, f.angleDeg, f.timeMs)
            falls.add(rec)
            if (f.event == BleProto.FALL_EVENT_CONFIRMED) {
                fallAlert.value = rec
            }
            postFallNotification(f)
        }
        // v3：原始运动流（0x12）→ 存档器入队（非阻塞）
        ble.motionListener = { sample ->
            motion.append(sample)
        }
    }

    // =====================================================================
    // 跌倒通知（v3 §1.3）
    // =====================================================================

    private fun postFallNotification(f: BleProto.FallEvent) {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            log.event("缺少通知权限，跳过跌倒通知")
            return
        }
        val nm = getSystemService(NotificationManager::class.java)
        if (nm?.getNotificationChannel(FALL_CHANNEL) == null) {
            val ch = NotificationChannel(
                FALL_CHANNEL, "跌倒告警",
                if (f.event == BleProto.FALL_EVENT_CONFIRMED) NotificationManager.IMPORTANCE_HIGH else NotificationManager.IMPORTANCE_DEFAULT,
            )
            if (f.event == BleProto.FALL_EVENT_CONFIRMED) {
                // 闹钟级：自定义铃声音量拉满 + 急促振动
                ch.setSound(RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM), android.media.AudioAttributes.Builder()
                    .setUsage(android.media.AudioAttributes.USAGE_ALARM)
                    .setContentType(android.media.AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build())
                ch.enableVibration(true)
                ch.vibrationPattern = longArrayOf(0, 800, 400, 800, 400, 800)
            }
            nm?.createNotificationChannel(ch)
        }
        val intent = Intent(this, MainActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            putExtra(EXTRA_FALL_ALERT, true)
        }
        val pi = PendingIntent.getActivity(
            this, 2002, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val confirmed = f.event == BleProto.FALL_EVENT_CONFIRMED
        val title = when (f.event) {
            BleProto.FALL_EVENT_CONFIRMED -> "疑似跌倒！"
            BleProto.FALL_EVENT_CANCELED -> "跌倒告警已取消"
            BleProto.FALL_EVENT_TEST -> "跌倒测试事件"
            else -> "跌倒事件(${f.event})"
        }
        val text = when (f.event) {
            BleProto.FALL_EVENT_CONFIRMED ->
                "冲击 ${f.impactMg}mg · 角度 ${f.angleDeg}° · ${fmtTime(f.timeMs)}，点按查看"
            BleProto.FALL_EVENT_CANCELED -> "倒计时内已取消，无需处理"
            BleProto.FALL_EVENT_TEST -> "联调测试：${f.impactMg}mg / ${f.angleDeg}°（${fmtTime(f.timeMs)}）"
            else -> ""
        }
        val n = NotificationCompat.Builder(this, FALL_CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_watch)
            .setContentTitle(title)
            .setContentText(text)
            .setPriority(if (confirmed) NotificationCompat.PRIORITY_MAX else NotificationCompat.PRIORITY_DEFAULT)
            .setCategory(if (confirmed) NotificationCompat.CATEGORY_ALARM else NotificationCompat.CATEGORY_RECOMMENDATION)
            .setContentIntent(pi)
            .setAutoCancel(true)
            .build()
        runCatching { NotificationManagerCompat.from(this).notify(FALL_NOTIF_ID, n) }
        log.event("跌倒通知已发送（${f.label}）")
    }

    private fun fmtTime(ms: Long): String =
        SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date(ms))

    companion object {
        const val FALL_CHANNEL = "fall_alert"
        const val FALL_NOTIF_ID = 2002
        const val EXTRA_FALL_ALERT = "fall_alert"
    }
}
