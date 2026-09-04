package com.aiwatch.companion.ui

import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ReceiptLong
import androidx.compose.material.icons.outlined.Dashboard
import androidx.compose.material.icons.outlined.DirectionsRun
import androidx.compose.material.icons.outlined.Emergency
import androidx.compose.material.icons.outlined.Mic
import androidx.compose.material.icons.outlined.Notifications
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.aiwatch.companion.WatchApp
import com.aiwatch.companion.ble.BleProto
import com.aiwatch.companion.data.FallRecord
import com.aiwatch.companion.ui.screens.AiScreen
import com.aiwatch.companion.ui.screens.DashboardScreen
import com.aiwatch.companion.ui.screens.LogsScreen
import com.aiwatch.companion.ui.screens.RemindersScreen
import com.aiwatch.companion.ui.screens.SettingsScreen
import com.aiwatch.companion.ui.screens.SportScreen

/**
 * 应用根：权限门 → 主界面（底部导航六页：仪表盘 / 运动 / AI / 提醒 / 日志 / 设置）。
 */
@Composable
fun AppRoot() {
    val app = LocalContext.current.applicationContext as WatchApp

    var permsOk by remember { mutableStateOf(app.ble.hasBlePermissions()) }
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                permsOk = app.ble.hasBlePermissions()
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    if (!permsOk) {
        PermissionsGate(onRecheck = { permsOk = app.ble.hasBlePermissions() })
        return
    }
    MainApp(app)
}

@Composable
private fun MainApp(app: WatchApp) {
    var tab by rememberSaveable { mutableStateOf(0) }
    val snackbarHostState = remember { SnackbarHostState() }

    LaunchedEffect(Unit) {
        app.ble.events.collect { snackbarHostState.showSnackbar(it) }
    }

    // 首次进入自动发起扫描连接
    var autoStarted by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        if (!autoStarted) {
            autoStarted = true
            app.ble.scanAndConnect()
        }
    }

    // 后台 AI 触发通知点击进入 → 消费待启动触发，直接开始聆听
    LaunchedEffect(Unit) {
        app.ai.pendingAutoStart.collect { t ->
            if (t != null) {
                app.ai.pendingAutoStart.value = null
                tab = 2 // 切到 AI 页
                app.ai.startSession(t.source, t.hr, t.spo2)
            }
        }
    }

    // 手表 AI 触发（App 前台）→ 新会话一开始立即切到 AI 页
    val aiActive by app.ai.active.collectAsStateWithLifecycle()
    LaunchedEffect(aiActive?.id) {
        if (aiActive != null) tab = 2
    }

    // v3：跌倒告警（event=1 前台弹窗 / 通知点击进入）
    val fallAlert by app.fallAlert.collectAsStateWithLifecycle()
    if (fallAlert != null && fallAlert?.event == BleProto.FALL_EVENT_CONFIRMED) {
        FallAlertDialog(
            fall = fallAlert!!,
            onDismiss = { app.fallAlert.value = null },
            onOpenHistory = {
                app.fallAlert.value = null
                tab = 1 // 运动页
            },
            onCallEmergency = { app.ble.notifyUser("紧急联系人拨打功能本期未接入（占位）") },
        )
    }

    val tabs = listOf(
        "仪表盘" to Icons.Outlined.Dashboard,
        "运动" to Icons.Outlined.DirectionsRun,
        "AI" to Icons.Outlined.Mic,
        "提醒" to Icons.Outlined.Notifications,
        "日志" to Icons.AutoMirrored.Outlined.ReceiptLong,
        "设置" to Icons.Outlined.Settings,
    )

    Scaffold(
        snackbarHost = { SnackbarHost(snackbarHostState) },
        bottomBar = {
            NavigationBar {
                tabs.forEachIndexed { i, (label, icon) ->
                    NavigationBarItem(
                        selected = tab == i,
                        onClick = { tab = i },
                        icon = { Icon(icon, contentDescription = label) },
                        label = { Text(label) },
                    )
                }
            }
        },
    ) { padding ->
        when (tab) {
            0 -> DashboardScreen(padding, onGoToSport = { tab = 1 })
            1 -> SportScreen(padding)
            2 -> AiScreen(padding, onGoToSettings = { tab = 5 })
            3 -> RemindersScreen(padding)
            4 -> LogsScreen(padding)
            5 -> SettingsScreen(padding)
        }
    }
}

/** 疑似跌倒全屏置顶告警（event=1）：展示冲击/角度/时间，"我没事"与"拨打紧急联系人"（本期 UI 占位） */
@Composable
private fun FallAlertDialog(
    fall: FallRecord,
    onDismiss: () -> Unit,
    onOpenHistory: () -> Unit,
    onCallEmergency: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = MaterialTheme.colorScheme.errorContainer,
        title = {
            Text(
                "疑似跌倒！",
                color = MaterialTheme.colorScheme.onErrorContainer,
                fontWeight = FontWeight.Bold,
            )
        },
        text = {
            androidx.compose.foundation.layout.Column {
                Text(
                    "手表检测到疑似跌倒（非医疗级判断）。\n\n" +
                        "冲击峰值：${fall.impactMg} mg（${"%.2f".format(fall.impactMg / 1000.0)} g）\n" +
                        "姿态角变化：${fall.angleDeg}°\n" +
                        "时间：${formatDateTime(fall.timeMs)}\n\n" +
                        "您还好吗？",
                    color = MaterialTheme.colorScheme.onErrorContainer,
                )
                androidx.compose.foundation.layout.Spacer(Modifier.padding(top = 8.dp))
                TextButton(onClick = onCallEmergency) {
                    Text("拨打紧急联系人（本期占位）", color = MaterialTheme.colorScheme.onErrorContainer)
                }
            }
        },
        confirmButton = {
            Button(onClick = onDismiss) {
                Text("我没事")
            }
        },
        dismissButton = {
            OutlinedButton(onClick = onOpenHistory) {
                Text("查看记录")
            }
        },
    )
}
