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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.DeleteSweep
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.aiwatch.companion.WatchApp
import com.aiwatch.companion.ble.LogDir
import com.aiwatch.companion.ble.LogEntry
import com.aiwatch.companion.ble.WatchLog

/**
 * 联调日志面板：原始帧 hex + 事件，按方向着色，可过滤/复制/清空。
 */
@Composable
fun LogsScreen(padding: PaddingValues) {
    val app = LocalContext.current.applicationContext as WatchApp
    val entries by app.log.entries.collectAsStateWithLifecycle()
    val clipboard = LocalClipboardManager.current

    var filter by rememberSaveable { mutableStateOf(0) } // 0=全部 1=收发帧 2=错误/事件

    val filtered = remember(entries, filter) {
        when (filter) {
            1 -> entries.filter { it.dir == LogDir.TX || it.dir == LogDir.RX }
            2 -> entries.filter { it.dir == LogDir.ERROR || it.dir == LogDir.EVENT }
            else -> entries
        }.asReversed() // 最新在最上
    }

    Column(
        modifier = Modifier
            .padding(padding)
            .fillMaxSize()
            .padding(horizontal = 16.dp),
    ) {
        Spacer(Modifier.height(8.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                "联调日志",
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.weight(1f),
            )
            IconButton(onClick = {
                clipboard.setText(AnnotatedString(app.log.copyAllText()))
            }) {
                Icon(Icons.Outlined.ContentCopy, "复制全部日志")
            }
            IconButton(onClick = { app.log.clear() }) {
                Icon(Icons.Outlined.DeleteSweep, "清空日志")
            }
        }

        Spacer(Modifier.height(4.dp))
        SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
            SegmentedButton(
                selected = filter == 0,
                onClick = { filter = 0 },
                shape = SegmentedButtonDefaults.itemShape(index = 0, count = 3),
            ) { Text("全部 ${entries.size}") }
            SegmentedButton(
                selected = filter == 1,
                onClick = { filter = 1 },
                shape = SegmentedButtonDefaults.itemShape(index = 1, count = 3),
            ) { Text("收发帧") }
            SegmentedButton(
                selected = filter == 2,
                onClick = { filter = 2 },
                shape = SegmentedButtonDefaults.itemShape(index = 2, count = 3),
            ) { Text("事件/错误") }
        }

        Spacer(Modifier.height(8.dp))
        if (filtered.isEmpty()) {
            Text(
                "暂无日志。连接手表后，这里会显示原始帧 hex（↑发送 / ↓接收）与事件。",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.outline,
                modifier = Modifier.padding(vertical = 32.dp),
            )
        } else {
            LazyColumn(
                verticalArrangement = Arrangement.spacedBy(6.dp),
                contentPadding = PaddingValues(bottom = 24.dp),
            ) {
                items(filtered) { e -> LogRow(e) }
            }
        }
    }
}

@Composable
private fun LogRow(e: LogEntry) {
    val (dirText, dirColor) = when (e.dir) {
        LogDir.TX -> "↑TX" to MaterialTheme.colorScheme.primary
        LogDir.RX -> "↓RX" to Color_Green
        LogDir.EVENT -> " · " to MaterialTheme.colorScheme.onSurfaceVariant
        LogDir.ERROR -> " ✗ " to MaterialTheme.colorScheme.error
    }
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.Top) {
            Text(
                WatchLog.formatTime(e.timeMs),
                style = MaterialTheme.typography.labelSmall,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.outline,
            )
            Spacer(Modifier.width(8.dp))
            Text(
                dirText,
                style = MaterialTheme.typography.labelSmall,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold,
                color = dirColor,
            )
            Spacer(Modifier.width(8.dp))
            if (e.tag.isNotEmpty()) {
                Text(
                    e.tag,
                    style = MaterialTheme.typography.labelSmall,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.width(8.dp))
            Text(
                e.note,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.weight(1f),
            )
        }
        e.hex?.let { hex ->
            Text(
                hex,
                style = MaterialTheme.typography.labelSmall,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 16.dp),
            )
        }
    }
}

private val Color_Green = androidx.compose.ui.graphics.Color(0xFF2E7D32)
