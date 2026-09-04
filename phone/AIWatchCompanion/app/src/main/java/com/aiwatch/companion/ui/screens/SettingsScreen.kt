package com.aiwatch.companion.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Bolt
import androidx.compose.material.icons.outlined.HealthAndSafety
import androidx.compose.material.icons.outlined.Info
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.outlined.Sync
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.aiwatch.companion.WatchApp
import com.aiwatch.companion.ble.BleProto
import com.aiwatch.companion.ble.Phase
import com.aiwatch.companion.ui.formatUtcSeconds
import kotlinx.coroutines.launch

/**
 * 设置：前台服务保活 / 时间同步 / 协议信息 / 关于。
 */
@Composable
fun SettingsScreen(padding: PaddingValues) {
    val app = LocalContext.current.applicationContext as WatchApp
    val keepAlive by app.settings.keepAlive.collectAsStateWithLifecycle(initialValue = true)
    val state by app.ble.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()

    Column(
        modifier = Modifier
            .padding(padding)
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp),
    ) {
        Spacer(Modifier.height(8.dp))
        Text("设置", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)

        Spacer(Modifier.height(12.dp))

        // ---------- AI 服务（v2：LLM / ASR / TTS 三组独立配置） ----------
        AiSettingsSection(app)

        Spacer(Modifier.height(12.dp))

        // ---------- 连接保活 ----------
        SectionCard(title = "连接", icon = Icons.Outlined.Bolt) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text("前台服务保活连接", style = MaterialTheme.typography.bodyLarge, fontWeight = FontWeight.Medium)
                    Text(
                        "开启后在后台保持与手表的连接与自动重连（前台服务 + 常驻通知）",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Switch(
                    checked = keepAlive,
                    onCheckedChange = { v ->
                        scope.launch {
                            app.settings.setKeepAlive(v)
                            app.ble.setKeepAlive(v)
                        }
                    },
                )
            }
        }

        Spacer(Modifier.height(12.dp))

        // ---------- 时间同步 ----------
        SectionCard(title = "时间同步", icon = Icons.Outlined.Sync) {
            Text(
                "手表 RTC 永远存 UTC，时区只用于本地显示。以下时机自动校时：",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(8.dp))
            BulletText("连接建立且 MTU 协商完成后立即同步")
            BulletText("系统时区/时间变化时补发")
            BulletText("每 8 小时兜底重发")
            BulletText("手表侧 last_sync：${formatUtcSeconds(state.lastSyncUtc)}")
            Spacer(Modifier.height(12.dp))
            Button(
                onClick = { scope.launch { app.ble.syncTimeNow() } },
                enabled = state.phase == Phase.CONNECTED,
            ) { Text("立即同步时间") }
        }

        Spacer(Modifier.height(12.dp))

        // ---------- 协议信息 ----------
        SectionCard(title = "协议信息（v1 / 0x01）", icon = Icons.Outlined.Info) {
            MonoRow("服务", BleProto.SERVICE_UUID.toString())
            MonoRow("Status f1", "${BleProto.STATUS_UUID} · Read+Notify · 手表→手机")
            MonoRow("TimeSync f2", "${BleProto.TIME_SYNC_UUID} · Write · 手机→手表")
            MonoRow("DataUpload f3", "${BleProto.DATA_UPLOAD_UUID} · Notify · 手表→手机")
            MonoRow("Command f4", "${BleProto.COMMAND_UUID} · Write · 手机→手表")
            Spacer(Modifier.height(8.dp))
            Text(
                "多字节字段一律小端序；扫描过滤：名称前缀 ${BleProto.DEVICE_NAME_PREFIX} 或广播内服务 UUID；" +
                    "MTU 协商 ≥247；断线重连 2s→30s 指数退避；提醒标题 ≤24 字节（UTF-8 字符边界截断），手表最多存 8 条。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        Spacer(Modifier.height(12.dp))

        // ---------- 关于 ----------
        SectionCard(title = "关于", icon = Icons.Outlined.Settings) {
            BulletText("AI Watch 手机配套应用 v1.0.0")
            BulletText("角色：BLE Central（主机），手表为 Peripheral（从机）")
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    Icons.Outlined.HealthAndSafety,
                    null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.padding(top = 2.dp).height(16.dp),
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    "传感器数据仅供参考，非医疗用途。",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        Spacer(Modifier.height(24.dp))
    }
}

// ---------------------------------------------------------------------

@Composable
private fun SectionCard(title: String, icon: ImageVector, content: @Composable () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(icon, null, tint = MaterialTheme.colorScheme.primary)
                Spacer(Modifier.width(8.dp))
                Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            }
            Spacer(Modifier.height(12.dp))
            content()
        }
    }
}

@Composable
private fun BulletText(text: String) {
    Text(
        "· $text",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurface,
        modifier = Modifier.padding(vertical = 2.dp),
    )
}

@Composable
private fun MonoRow(label: String, value: String) {
    Column(Modifier.padding(vertical = 4.dp)) {
        Text(label, style = MaterialTheme.typography.labelMedium, fontWeight = FontWeight.SemiBold)
        Text(
            value,
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
