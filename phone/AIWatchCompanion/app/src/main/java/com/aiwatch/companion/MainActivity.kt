package com.aiwatch.companion

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.aiwatch.companion.ai.AiSessionManager
import com.aiwatch.companion.ui.AppRoot
import com.aiwatch.companion.ui.WatchTheme

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        handleAiIntent(intent)
        handleFallIntent(intent)
        setContent {
            WatchTheme {
                AppRoot()
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleAiIntent(intent)
        handleFallIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        (application as WatchApp).ai.appInForeground = true
    }

    override fun onPause() {
        super.onPause()
        (application as WatchApp).ai.appInForeground = false
    }

    override fun onStart() {
        super.onStart()
        // 应用回前台：立即触发一轮重连（若处于重连等待）
        (application as WatchApp).ble.onAppForeground()
    }

    /** 后台 AI 触发通知点击进入：携带触发参数，UI 层消费后直接开始聆听 */
    private fun handleAiIntent(intent: Intent?) {
        val i = intent ?: return
        val source = i.getIntExtra(AiSessionManager.EXTRA_AI_SOURCE, -1)
        if (source >= 0) {
            (application as WatchApp).ai.pendingAutoStart.value = com.aiwatch.companion.ai.TriggerInfo(
                source = source,
                hr = i.getIntExtra(AiSessionManager.EXTRA_AI_HR, 0),
                spo2 = i.getIntExtra(AiSessionManager.EXTRA_AI_SPO2, 0),
            )
        }
    }

    /** 跌倒通知点击进入：置顶展示最近一条跌倒事件 */
    private fun handleFallIntent(intent: Intent?) {
        if (intent?.getBooleanExtra(WatchApp.EXTRA_FALL_ALERT, false) == true) {
            val app = application as WatchApp
            app.fallAlert.value = app.falls.latest
        }
    }
}
