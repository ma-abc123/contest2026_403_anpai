package com.aiwatch.companion.ui

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.aiwatch.companion.ble.Phase
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** ---------- 格式化 ---------- */

private val HMS = SimpleDateFormat("HH:mm:ss", Locale.getDefault())
private val DATETIME = SimpleDateFormat("MM-dd HH:mm:ss", Locale.getDefault())
private val FULL = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault())

fun formatHms(ms: Long): String = synchronized(HMS) { HMS.format(Date(ms)) }
fun formatDateTime(ms: Long): String = synchronized(DATETIME) { DATETIME.format(Date(ms)) }
fun formatFull(ms: Long): String = synchronized(FULL) { FULL.format(Date(ms)) }

fun formatUtcSeconds(sec: Long): String =
    if (sec <= 0) "从未同步" else formatFull(sec * 1000)

/** ---------- 连接阶段文案 / 颜色 ---------- */

fun phaseLabel(p: Phase): String = when (p) {
    Phase.IDLE -> "未连接"
    Phase.SCANNING -> "扫描中"
    Phase.CONNECTING -> "连接中"
    Phase.INITIALIZING -> "初始化（MTU/订阅）"
    Phase.CONNECTED -> "已连接"
    Phase.RECONNECTING -> "重连中"
    Phase.BLUETOOTH_OFF -> "蓝牙已关闭"
}

fun isPhaseActive(p: Phase): Boolean =
    p == Phase.SCANNING || p == Phase.CONNECTING || p == Phase.INITIALIZING || p == Phase.RECONNECTING

@Composable
fun phaseColor(p: Phase): Color = when (p) {
    Phase.CONNECTED -> Color(0xFF2E7D32)
    Phase.RECONNECTING -> Color(0xFFEF6C00)
    Phase.SCANNING, Phase.CONNECTING, Phase.INITIALIZING -> MaterialTheme.colorScheme.tertiary
    Phase.BLUETOOTH_OFF -> MaterialTheme.colorScheme.error
    Phase.IDLE -> MaterialTheme.colorScheme.outline
}

/** 状态圆点 */
@Composable
fun StatusDot(color: Color, pulse: Boolean, modifier: Modifier = Modifier) {
    val alpha: Float = if (pulse) {
        val transition = rememberInfiniteTransition(label = "pulse")
        transition.animateFloat(
            initialValue = 0.35f,
            targetValue = 1f,
            animationSpec = infiniteRepeatable(
                animation = tween(600),
                repeatMode = RepeatMode.Reverse,
            ),
            label = "pulseAlpha",
        ).value
    } else {
        1f
    }
    Box(
        modifier = modifier
            .size(12.dp)
            .alpha(alpha)
            .background(color, CircleShape),
    )
}
