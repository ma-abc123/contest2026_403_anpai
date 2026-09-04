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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Air
import androidx.compose.material.icons.outlined.Bluetooth
import androidx.compose.material.icons.outlined.BluetoothDisabled
import androidx.compose.material.icons.outlined.BluetoothSearching
import androidx.compose.material.icons.outlined.DeviceThermostat
import androidx.compose.material.icons.outlined.DirectionsRun
import androidx.compose.material.icons.outlined.DirectionsWalk
import androidx.compose.material.icons.outlined.Emergency
import androidx.compose.material.icons.outlined.HealthAndSafety
import androidx.compose.material.icons.outlined.Link
import androidx.compose.material.icons.outlined.LinkOff
import androidx.compose.material.icons.outlined.MonitorHeart
import androidx.compose.material.icons.outlined.Sync
import androidx.compose.material.icons.outlined.WaterDrop
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.aiwatch.companion.WatchApp
import com.aiwatch.companion.ble.BleProto
import com.aiwatch.companion.ble.Phase
import com.aiwatch.companion.ble.SensorValue
import com.aiwatch.companion.ble.WatchState
import com.aiwatch.companion.ui.StatusDot
import com.aiwatch.companion.ui.formatHms
import com.aiwatch.companion.ui.formatUtcSeconds
import com.aiwatch.companion.ui.isPhaseActive
import com.aiwatch.companion.ui.phaseColor
import com.aiwatch.companion.ui.phaseLabel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import androidx.compose.ui.platform.LocalContext

/**
 * 仪表盘：连接状态 / 扫描 / 传感器卡片 / 免责声明。
 */
@Composable
fun DashboardScreen(padding: PaddingValues, onGoToSport: () -> Unit) {
    val app = LocalContext.current.applicationContext as WatchApp
    val state by app.ble.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()

    // 重连倒计时
    var nowMs by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(state.phase) {
        while (state.phase == Phase.RECONNECTING) {
            nowMs = System.currentTimeMillis()
            delay(500)
        }
    }

    Column(
        modifier = Modifier
            .padding(padding)
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp),
    ) {
        Spacer(Modifier.height(8.dp))
        ConnectionCard(
            state = state,
            nowMs = nowMs,
            onPrimary = {
                when (state.phase) {
                    Phase.IDLE -> app.ble.scanAndConnect()
                    else -> app.ble.disconnectByUser()
                }
            },
            onSyncTime = {
                scope.launch { app.ble.syncTimeNow() }
            },
        )

        if (state.phase == Phase.SCANNING && state.scanHits.isNotEmpty()) {
            Spacer(Modifier.height(12.dp))
            ScanHitsCard(hits = state.scanHits, onConnect = { app.ble.connectToAddress(it) })
        }

        Spacer(Modifier.height(16.dp))
        Text(
            "运动数据",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            MotionCard(
                modifier = Modifier.weight(1f),
                label = "今日步数",
                icon = Icons.Outlined.DirectionsWalk,
                value = state.steps.toString(),
                sub = if (state.phase == Phase.CONNECTED) "连接后自动上报" else "未连接",
            )
            MotionCard(
                modifier = Modifier.weight(1f),
                label = "活动状态",
                icon = Icons.Outlined.DirectionsRun,
                value = BleProto.activityName(state.activity),
                sub = "0x06 分类变化时上报",
            )
        }
        Spacer(Modifier.height(12.dp))
        state.fallEvent?.let { fall ->
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(
                    containerColor = if (fall.event == BleProto.FALL_EVENT_CONFIRMED) MaterialTheme.colorScheme.errorContainer
                    else MaterialTheme.colorScheme.surfaceVariant,
                ),
            ) {
                Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        Icons.Outlined.Emergency,
                        null,
                        tint = if (fall.event == BleProto.FALL_EVENT_CONFIRMED) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.primary,
                        modifier = Modifier.size(22.dp),
                    )
                    Spacer(Modifier.width(10.dp))
                    Column(Modifier.weight(1f)) {
                        Text(
                            fall.label,
                            style = MaterialTheme.typography.titleSmall,
                            fontWeight = FontWeight.SemiBold,
                            color = if (fall.event == BleProto.FALL_EVENT_CONFIRMED) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
                        )
                        Text(
                            "冲击 ${fall.impactMg}mg · 角度 ${fall.angleDeg}° · ${formatHms(fall.timeMs)}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    TextButton(onClick = onGoToSport) { Text("查看记录") }
                }
            }
        } ?: Card(modifier = Modifier.fillMaxWidth()) {
            Text(
                "暂无跌倒事件。手表端可到 Exercise 页点 Test 联调演练。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(16.dp),
            )
        }

        Spacer(Modifier.height(16.dp))
        Text(
            "传感器数据",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            SensorCard(
                modifier = Modifier.weight(1f),
                label = "温度",
                unit = "℃",
                icon = Icons.Outlined.DeviceThermostat,
                value = state.sensors.temperature,
                format = { "%.1f".format(it) },
            )
            SensorCard(
                modifier = Modifier.weight(1f),
                label = "心率",
                unit = "次/分",
                icon = Icons.Outlined.MonitorHeart,
                value = state.sensors.heartRate,
                format = { "%.0f".format(it) },
            )
        }
        Spacer(Modifier.height(12.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            SensorCard(
                modifier = Modifier.weight(1f),
                label = "湿度",
                unit = "%RH",
                icon = Icons.Outlined.WaterDrop,
                value = state.sensors.humidity,
                format = { "%.1f".format(it) },
            )
            SensorCard(
                modifier = Modifier.weight(1f),
                label = "血氧",
                unit = "%",
                icon = Icons.Outlined.Air,
                value = state.sensors.spo2,
                format = { "%.0f".format(it) },
            )
        }

        Spacer(Modifier.height(12.dp))
        DisclaimerCard()
        Spacer(Modifier.height(24.dp))
    }
}

// ---------------------------------------------------------------------

@Composable
private fun ConnectionCard(state: WatchState, nowMs: Long, onPrimary: () -> Unit, onSyncTime: () -> Unit) {
    val color = phaseColor(state.phase)
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
    ) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                StatusDot(color = color, pulse = isPhaseActive(state.phase))
                Spacer(Modifier.width(10.dp))
                Column(Modifier.weight(1f)) {
                    Text(phaseLabel(state.phase), style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
                    Text(
                        state.deviceName ?: "未选择设备 (${BleProto.DEVICE_NAME_PREFIX})",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Icon(
                    imageVector = when (state.phase) {
                        Phase.CONNECTED -> Icons.Outlined.Bluetooth
                        Phase.BLUETOOTH_OFF -> Icons.Outlined.BluetoothDisabled
                        Phase.SCANNING -> Icons.Outlined.BluetoothSearching
                        else -> Icons.Outlined.BluetoothSearching
                    },
                    contentDescription = null,
                    tint = color,
                    modifier = Modifier.size(28.dp),
                )
            }

            Spacer(Modifier.height(12.dp))
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            Spacer(Modifier.height(12.dp))

            InfoRow("手表地址", state.deviceAddress ?: "--")
            InfoRow("上次校时（手表侧）", formatUtcSeconds(state.lastSyncUtc))
            InfoRow("MTU", if (state.phase == Phase.CONNECTED) "${state.mtu} 字节" else "--")
            if (state.queueSize > 0) InfoRow("待发送队列", "${state.queueSize} 条")

            if (state.phase == Phase.RECONNECTING) {
                val remain = ((state.nextRetryAtMs - nowMs).coerceAtLeast(0) / 1000.0)
                InfoRow("自动重连", "第 ${state.retryAttempt} 次尝试，${"%.0f".format(remain)}s 后（2s→30s 指数退避）")
            }

            Spacer(Modifier.height(16.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Button(
                    onClick = onPrimary,
                    modifier = Modifier.weight(1f),
                    enabled = state.phase != Phase.BLUETOOTH_OFF,
                ) {
                    Text(
                        when (state.phase) {
                            Phase.IDLE -> "扫描并连接"
                            Phase.SCANNING -> "停止扫描"
                            Phase.CONNECTING, Phase.INITIALIZING -> "取消连接"
                            Phase.CONNECTED -> "断开"
                            Phase.RECONNECTING -> "停止重连"
                            Phase.BLUETOOTH_OFF -> "蓝牙已关闭"
                        },
                    )
                }
                OutlinedButton(
                    onClick = onSyncTime,
                    enabled = state.phase == Phase.CONNECTED,
                ) {
                    Icon(Icons.Outlined.Sync, null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text("同步时间")
                }
            }
            if (state.phase == Phase.BLUETOOTH_OFF) {
                Spacer(Modifier.height(8.dp))
                Text(
                    "请在系统快捷设置中打开蓝牙，应用会自动恢复连接",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
        }
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
        Text(
            label,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(1f),
        )
        Text(value, style = MaterialTheme.typography.bodySmall, fontWeight = FontWeight.Medium)
    }
}

@Composable
private fun ScanHitsCard(hits: List<com.aiwatch.companion.ble.ScanHit>, onConnect: (String) -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
            Text(
                "发现的设备（点击连接）",
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.SemiBold,
            )
            hits.forEach { hit ->
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(modifier = Modifier.weight(1f).padding(vertical = 6.dp)) {
                        Text(hit.name ?: "未知名称", style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium)
                        Text(
                            "${hit.address} · ${hit.rssi} dBm",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    TextButton(onClick = { onConnect(hit.address) }) { Text("连接") }
                }
            }
        }
    }
}

@Composable
private fun SensorCard(
    modifier: Modifier = Modifier,
    label: String,
    unit: String,
    icon: ImageVector,
    value: SensorValue?,
    format: (Float) -> String,
) {
    Card(modifier = modifier) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(icon, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(20.dp))
                Spacer(Modifier.width(8.dp))
                Text(label, style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Spacer(Modifier.height(10.dp))
            Text(
                value?.let { format(it.value) } ?: "--",
                style = MaterialTheme.typography.displaySmall,
                fontWeight = FontWeight.Bold,
                color = if (value != null) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.outline,
            )
            Text(
                unit,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(6.dp))
            Text(
                value?.let { "更新于 ${formatHms(it.atMs)}" } ?: "等待手表推送…",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.outline,
            )
        }
    }
}

@Composable
private fun MotionCard(
    modifier: Modifier = Modifier,
    label: String,
    icon: ImageVector,
    value: String,
    sub: String,
) {
    Card(modifier = modifier) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(icon, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(20.dp))
                Spacer(Modifier.width(8.dp))
                Text(label, style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Spacer(Modifier.height(10.dp))
            Text(
                value,
                style = MaterialTheme.typography.headlineMedium,
                fontWeight = FontWeight.Bold,
            )
            Text(
                sub,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.outline,
            )
        }
    }
}

@Composable
private fun DisclaimerCard() {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.tertiaryContainer),
    ) {
        Row(
            modifier = Modifier.padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(
                Icons.Outlined.HealthAndSafety,
                null,
                tint = MaterialTheme.colorScheme.onTertiaryContainer,
                modifier = Modifier.size(20.dp),
            )
            Spacer(Modifier.width(10.dp))
            Text(
                "数据仅供参考，非医疗用途。心率/血氧仅在手表对应页面且数据质量达标时推送；未收到的数据以 -- 显示。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onTertiaryContainer,
            )
        }
    }
}
