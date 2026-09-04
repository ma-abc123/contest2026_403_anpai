package com.aiwatch.companion.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Alarm
import androidx.compose.material.icons.outlined.AlarmOn
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.Mic
import androidx.compose.material.icons.outlined.NotificationsActive
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.SmartToy
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.aiwatch.companion.WatchApp
import com.aiwatch.companion.ble.BleProto
import com.aiwatch.companion.ble.Phase
import com.aiwatch.companion.ai.AiActionRecord
import com.aiwatch.companion.ai.AiConfig
import com.aiwatch.companion.ai.AiSessionRecord
import com.aiwatch.companion.ai.AiStage
import com.aiwatch.companion.ui.formatDateTime
import androidx.compose.foundation.lazy.items

/**
 * AI 会话页：触发状态 / 手动开始 / 最近会话列表（含动作结果与重试）。
 */
@Composable
fun AiScreen(padding: PaddingValues, onGoToSettings: () -> Unit) {
    val app = LocalContext.current.applicationContext as WatchApp
    val active by app.ai.active.collectAsStateWithLifecycle()
    val sessions by app.ai.sessions.collectAsStateWithLifecycle()
    val micLevel by app.ai.micLevel.collectAsStateWithLifecycle()
    val config by app.ai.configRepo.config.collectAsStateWithLifecycle(initialValue = AiConfig())
    val bleState by app.ble.state.collectAsStateWithLifecycle()
    val connected = bleState.phase == Phase.CONNECTED

    Column(
        modifier = Modifier
            .padding(padding)
            .fillMaxSize()
            .padding(horizontal = 16.dp),
    ) {
        Spacer(Modifier.height(8.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("AI 语音助手", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                Text(
                    when (bleState.phase) {
                        Phase.CONNECTED -> "手表已连接 · 说话内容仅发往你配置的服务"
                        else -> "手表未连接，无法接收触发或下发结果"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (active == null) {
                Button(onClick = { app.ai.startSession(BleProto.AI_SRC_MANUAL, 0, 0) }, enabled = connected) {
                    Icon(Icons.Outlined.Mic, null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text("开始对话")
                }
            } else {
                TextButton(onClick = { app.ai.cancel() }) {
                    Icon(Icons.Outlined.Close, null, modifier = Modifier.size(18.dp))
                    Text("取消")
                }
            }
        }

        Spacer(Modifier.height(8.dp))

        // LLM 未配置引导
        if (!config.llm.configured && active == null) {
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text("AI 服务未就绪", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold, color = MaterialTheme.colorScheme.onErrorContainer)
                    Text(
                        "还没有配置 LLM（必配）。请到设置页选择 Base URL、填写 API Key、选择模型并测试通过。",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onErrorContainer,
                    )
                    TextButton(onClick = onGoToSettings) { Text("去设置") }
                }
            }
            Spacer(Modifier.height(12.dp))
        }

        // 活动会话
        active?.let { ActiveSessionCard(it, micLevel) }

        Spacer(Modifier.height(12.dp))
        var showClearConfirm by remember { mutableStateOf(false) }
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Text("最近会话", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
            if (sessions.isNotEmpty()) {
                TextButton(onClick = { showClearConfirm = true }) {
                    Icon(Icons.Outlined.Delete, null, modifier = Modifier.size(16.dp))
                    Spacer(Modifier.width(4.dp))
                    Text("清空")
                }
            }
        }
        Spacer(Modifier.height(8.dp))

        if (showClearConfirm) {
            AlertDialog(
                onDismissRequest = { showClearConfirm = false },
                title = { Text("清空全部会话？") },
                text = { Text("将删除 ${sessions.size} 条会话记录，此操作不可撤销。") },
                confirmButton = {
                    TextButton(onClick = {
                        app.ai.clearSessions()
                        showClearConfirm = false
                    }) { Text("清空", color = MaterialTheme.colorScheme.error) }
                },
                dismissButton = {
                    TextButton(onClick = { showClearConfirm = false }) { Text("取消") }
                },
            )
        }

        if (sessions.isEmpty() && active == null) {
            Column(
                modifier = Modifier.fillMaxWidth().padding(vertical = 40.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Icon(Icons.Outlined.SmartToy, null, tint = MaterialTheme.colorScheme.outline, modifier = Modifier.size(40.dp))
                Spacer(Modifier.height(8.dp))
                Text(
                    "还没有会话。在手表 AI 页按 Start，\n或点右上角「开始对话」",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.outline,
                    textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                )
            }
        } else {
            LazyColumn(
                verticalArrangement = Arrangement.spacedBy(10.dp),
                contentPadding = PaddingValues(bottom = 24.dp),
                modifier = Modifier.fillMaxWidth(),
            ) {
                items(sessions, key = { it.id }) { s ->
                    SessionCard(
                        s,
                        onRetry = { app.ai.retry(s.id) },
                        onDelete = { app.ai.deleteSession(s.id) },
                    )
                }
            }
        }
    }
}

// ---------------------------------------------------------------------

@Composable
private fun ActiveSessionCard(session: AiSessionRecord, micLevel: Float) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.secondaryContainer),
    ) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Outlined.SmartToy, null, tint = MaterialTheme.colorScheme.primary)
                Spacer(Modifier.width(8.dp))
                Text(
                    stageLabel(session.stage),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f),
                )
            }
            if (session.stage == AiStage.LISTENING.name) {
                Spacer(Modifier.height(10.dp))
                // 实时麦克风电平条
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(8.dp)
                        .clip(RoundedCornerShape(4.dp))
                        .background(MaterialTheme.colorScheme.surfaceVariant),
                ) {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth(micLevel.coerceIn(0.02f, 1f))
                            .height(8.dp)
                            .clip(RoundedCornerShape(4.dp))
                            .background(MaterialTheme.colorScheme.primary),
                    )
                }
                Spacer(Modifier.height(6.dp))
                Text(
                    "对麦克风说话，静音 1.2 秒后自动结束（最长 15 秒）",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSecondaryContainer,
                )
            } else if (session.stage == AiStage.RECOGNIZING.name || session.stage == AiStage.THINKING.name || session.stage == AiStage.DELIVERING.name) {
                Spacer(Modifier.height(10.dp))
                LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
            }
            session.userText?.let {
                Spacer(Modifier.height(8.dp))
                Text(
                    "「$it」",
                    style = MaterialTheme.typography.bodyMedium,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

@Composable
private fun SessionCard(session: AiSessionRecord, onRetry: () -> Unit, onDelete: () -> Unit) {
    val failed = session.stage == AiStage.FAILED.name
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = if (failed) CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)
        else CardDefaults.cardColors(),
    ) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    sourceLabel(session),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.primary,
                    fontWeight = FontWeight.SemiBold,
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    formatDateTime(session.timeMs),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.weight(1f))
                if (failed) {
                    Text(
                        "失败",
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.error,
                        fontWeight = FontWeight.SemiBold,
                    )
                    IconButton(onClick = onRetry) {
                        Icon(Icons.Outlined.Refresh, "重试")
                    }
                } else if (session.stage == AiStage.CANCELED.name) {
                    Text("已取消", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.outline)
                }
                IconButton(onClick = onDelete) {
                    Icon(
                        Icons.Outlined.Delete,
                        "删除该会话",
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.size(18.dp),
                    )
                }
            }

            session.userText?.let {
                Text(
                    "「$it」",
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Medium,
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            session.reply?.let {
                Text(
                    "→ $it",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 4,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            session.error?.let {
                Text(
                    "错误：$it",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
            if (session.actions.isNotEmpty()) {
                Spacer(Modifier.height(6.dp))
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    session.actions.forEach { ActionRow(it) }
                }
            }
        }
    }
}

@Composable
private fun ActionRow(action: AiActionRecord) {
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
        Icon(
            imageVector = when (action.type) {
                "create_reminder" -> Icons.Outlined.Alarm
                "start_timer" -> Icons.Outlined.AlarmOn
                "notify" -> Icons.Outlined.NotificationsActive
                else -> Icons.Outlined.SmartToy
            },
            contentDescription = null,
            modifier = Modifier.size(16.dp),
            tint = if (action.ok) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
        )
        Spacer(Modifier.width(6.dp))
        val label = when (action.type) {
            "create_reminder" -> "提醒"
            "start_timer" -> "倒计时"
            "notify" -> "通知"
            else -> "回复"
        }
        Text(
            "$label · ${action.title} ${action.param}",
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.weight(1f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        Text(
            action.result,
            style = MaterialTheme.typography.labelSmall,
            color = if (action.ok) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
        )
    }
}

// ---------------------------------------------------------------------

private fun stageLabel(stage: String): String = when (stage) {
    AiStage.LISTENING.name -> "聆听中…"
    AiStage.RECOGNIZING.name -> "识别中…"
    AiStage.THINKING.name -> "思考中…"
    AiStage.DELIVERING.name -> "发送中…"
    AiStage.DONE.name -> "已完成"
    AiStage.FAILED.name -> "失败"
    AiStage.CANCELED.name -> "已取消"
    else -> stage
}

private fun sourceLabel(s: AiSessionRecord): String = when (s.source) {
    BleProto.AI_SRC_ASSISTANT_PAGE -> "手表 AI 页"
    BleProto.AI_SRC_HR_PAGE -> buildString {
        append("心率页 Ask AI")
        append(" · 心率 ${if (s.hr > 0) "${s.hr}" else "--"} · 血氧 ${if (s.spo2 > 0) "${s.spo2}%" else "--"}")
    }
    else -> "手动"
}
