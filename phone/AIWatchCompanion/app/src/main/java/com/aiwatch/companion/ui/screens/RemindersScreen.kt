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
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.DeleteSweep
import androidx.compose.material.icons.outlined.Edit
import androidx.compose.material.icons.outlined.MarkEmailRead
import androidx.compose.material.icons.outlined.MarkEmailUnread
import androidx.compose.material.icons.outlined.Notifications
import androidx.compose.material.icons.outlined.NotificationsActive
import androidx.compose.material.icons.outlined.Send
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TimePicker
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.material3.rememberTimePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.aiwatch.companion.WatchApp
import com.aiwatch.companion.ble.BleProto
import com.aiwatch.companion.ble.Phase
import com.aiwatch.companion.data.ReminderRepository
import com.aiwatch.companion.ui.formatDateTime
import com.aiwatch.companion.ui.formatHms
import androidx.compose.ui.platform.LocalContext
import kotlinx.coroutines.launch
import java.util.Calendar

/**
 * 提醒管理：本地列表 + 发送到手表（新增/更新/已读同步/清除全部）+ 发送记录。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RemindersScreen(padding: PaddingValues) {
    val app = LocalContext.current.applicationContext as WatchApp
    val reminders by app.reminders.reminders.collectAsStateWithLifecycle()
    val records by app.reminders.records.collectAsStateWithLifecycle()
    val bleState by app.ble.state.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    val connected = bleState.phase == Phase.CONNECTED

    var subTab by rememberSaveable { mutableStateOf(0) } // 0=列表 1=记录
    var editing by remember { mutableStateOf<ReminderRepository.ReminderEntity?>(null) }
    var showAdd by remember { mutableStateOf(false) }
    var showClearAll by remember { mutableStateOf(false) }

    fun doSend(entity: ReminderRepository.ReminderEntity, flagsOverride: Int? = null) {
        scope.launch {
            if (!connected) {
                app.ble.emitEvent("未连接手表，请先在仪表盘连接")
                return@launch
            }
            val trigger = entity.triggerEpochSec ?: (System.currentTimeMillis() / 1000)
            val st = app.ble.sendCommand(
                entity.cmdType, entity.id,
                flagsOverride ?: entity.flags, trigger, entity.title,
            )
            val result = if (st == 0) "成功" else BleProto.gattStatusMessage(st)
            app.reminders.updateSendResult(entity.id, result, System.currentTimeMillis())
            app.reminders.addRecord(
                ReminderRepository.SendRecord(
                    timeMs = System.currentTimeMillis(),
                    title = entity.title,
                    cmdType = entity.cmdType,
                    result = result,
                ),
            )
            app.ble.emitEvent(if (st == 0) "已发送：${entity.title}" else "发送失败：$result")
        }
    }

    Scaffold(
        modifier = Modifier.padding(padding),
        floatingActionButton = {
            if (subTab == 0) {
                FloatingActionButton(onClick = { showAdd = true }) {
                    Icon(Icons.Outlined.Add, "新增提醒")
                }
            }
        },
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .padding(innerPadding)
                .fillMaxSize()
                .padding(horizontal = 16.dp),
        ) {
            Spacer(Modifier.height(8.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    "手表提醒",
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.weight(1f),
                )
                TextButton(onClick = { showClearAll = true }, enabled = connected) {
                    Icon(Icons.Outlined.DeleteSweep, null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(4.dp))
                    Text("清空手表")
                }
            }

            Spacer(Modifier.height(4.dp))
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                SegmentedButton(
                    selected = subTab == 0,
                    onClick = { subTab = 0 },
                    shape = SegmentedButtonDefaults.itemShape(index = 0, count = 2),
                ) { Text("提醒列表 (${reminders.size})") }
                SegmentedButton(
                    selected = subTab == 1,
                    onClick = { subTab = 1 },
                    shape = SegmentedButtonDefaults.itemShape(index = 1, count = 2),
                ) { Text("发送记录 (${records.size})") }
            }

            if (subTab == 0) {
                Spacer(Modifier.height(8.dp))
                Text(
                    "手表最多存 ${BleProto.WATCH_MAX_REMINDERS} 条；同 id 重发即覆盖更新；队列满会自动 2s 退避重试 5 次。",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(8.dp))
                if (reminders.isEmpty()) {
                    EmptyHint(text = "暂无提醒，点右下角 + 新建\n（发送时手表必须已连接）")
                } else {
                    LazyColumn(
                        verticalArrangement = Arrangement.spacedBy(10.dp),
                        contentPadding = PaddingValues(bottom = 96.dp),
                    ) {
                        items(reminders, key = { it.id }) { r ->
                            ReminderItem(
                                r = r,
                                connected = connected,
                                onSend = { doSend(r) },
                                onEdit = { editing = r },
                                onDelete = { app.reminders.delete(r.id) },
                                onToggleRead = {
                                    val newFlags = r.flags xor BleProto.FLAG_READ
                                    app.reminders.updateFlags(r.id, newFlags)
                                    // 已读状态同步：重发同 id（flags bit0=1）
                                    doSend(r.copy(flags = newFlags))
                                },
                            )
                        }
                    }
                }
            } else {
                Spacer(Modifier.height(8.dp))
                if (records.isEmpty()) {
                    EmptyHint(text = "暂无发送记录")
                } else {
                    LazyColumn(
                        verticalArrangement = Arrangement.spacedBy(10.dp),
                        contentPadding = PaddingValues(bottom = 96.dp),
                    ) {
                        items(records.size) { i ->
                            val rec = records[records.size - 1 - i] // 最新的在前
                            RecordItem(rec)
                        }
                    }
                }
            }
        }
    }

    // ---------------- 对话框 ----------------

    if (showAdd) {
        ReminderEditDialog(
            initial = null,
            allocateId = { app.reminders.allocateId() },
            onDismiss = { showAdd = false },
            onSave = { entity, sendNow ->
                showAdd = false
                app.reminders.upsert(entity)
                if (sendNow) doSend(entity)
            },
        )
    }

    editing?.let { current ->
        ReminderEditDialog(
            initial = current,
            allocateId = { current.id },
            onDismiss = { editing = null },
            onSave = { entity, sendNow ->
                editing = null
                app.reminders.upsert(entity)
                if (sendNow) doSend(entity)
            },
        )
    }

    if (showClearAll) {
        AlertDialog(
            onDismissRequest = { showClearAll = false },
            title = { Text("清空手表提醒？") },
            text = { Text("将发送「清除全部」命令(0x03)，手表上所有提醒/通知将被删除。本地列表不受影响。") },
            confirmButton = {
                TextButton(onClick = {
                    showClearAll = false
                    scope.launch {
                        val st = app.ble.clearAllOnWatch()
                        val result = if (st == 0) "成功" else BleProto.gattStatusMessage(st)
                        app.reminders.addRecord(
                            ReminderRepository.SendRecord(
                                timeMs = System.currentTimeMillis(),
                                title = "（清除全部）",
                                cmdType = BleProto.CMD_CLEAR_ALL,
                                result = result,
                            ),
                        )
                        app.ble.emitEvent(if (st == 0) "已清除手表全部提醒" else "清除失败：$result")
                    }
                }) { Text("清除") }
            },
            dismissButton = { TextButton(onClick = { showClearAll = false }) { Text("取消") } },
        )
    }
}

// ---------------------------------------------------------------------

@Composable
private fun ReminderItem(
    r: ReminderRepository.ReminderEntity,
    connected: Boolean,
    onSend: () -> Unit,
    onEdit: () -> Unit,
    onDelete: () -> Unit,
    onToggleRead: () -> Unit,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    if (r.cmdType == BleProto.CMD_NOTIFY) Icons.Outlined.NotificationsActive else Icons.Outlined.Notifications,
                    null,
                    tint = if (r.cmdType == BleProto.CMD_NOTIFY) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.primary,
                    modifier = Modifier.size(20.dp),
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    r.title,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    "#${r.id}",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.outline,
                )
            }
            Spacer(Modifier.height(6.dp))
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                AssistChip(
                    onClick = {},
                    enabled = false,
                    label = { Text(if (r.cmdType == BleProto.CMD_NOTIFY) "通知" else "提醒") },
                )
                AssistChip(
                    onClick = {},
                    enabled = false,
                    label = { Text(if (r.isRead) "已读" else "未读") },
                )
                AssistChip(
                    onClick = {},
                    enabled = false,
                    label = { Text(r.triggerEpochSec?.let { "触发 ${formatDateTime(it * 1000)}" } ?: "即时") },
                )
            }
            r.lastSendState?.let {
                Spacer(Modifier.height(4.dp))
                Text(
                    "上次发送：$it" + (r.lastSendAt?.let { t -> "（${formatHms(t)}）" } ?: ""),
                    style = MaterialTheme.typography.labelSmall,
                    color = if (it == "成功") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
                )
            }
            Spacer(Modifier.height(4.dp))
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                IconButton(onClick = onToggleRead, enabled = connected) {
                    Icon(
                        if (r.isRead) Icons.Outlined.MarkEmailRead else Icons.Outlined.MarkEmailUnread,
                        if (r.isRead) "标记未读并同步" else "标记已读并同步",
                    )
                }
                IconButton(onClick = onSend, enabled = connected) {
                    Icon(Icons.Outlined.Send, "发送/更新到手表")
                }
                IconButton(onClick = onEdit) { Icon(Icons.Outlined.Edit, "编辑") }
                IconButton(onClick = onDelete) { Icon(Icons.Outlined.Delete, "从本地删除") }
            }
        }
    }
}

@Composable
private fun RecordItem(rec: ReminderRepository.SendRecord) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (rec.result == "成功")
                MaterialTheme.colorScheme.surfaceVariant
            else
                MaterialTheme.colorScheme.errorContainer,
        ),
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    if (rec.cmdType == BleProto.CMD_CLEAR_ALL) "（清除全部命令）" else rec.title,
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Medium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    "${formatDateTime(rec.timeMs)} · ${if (rec.cmdType == BleProto.CMD_NOTIFY) "通知" else if (rec.cmdType == BleProto.CMD_CLEAR_ALL) "清空" else "提醒"}",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Text(
                rec.result,
                style = MaterialTheme.typography.labelMedium,
                fontWeight = FontWeight.SemiBold,
                color = if (rec.result == "成功") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
            )
        }
    }
}

@Composable
private fun EmptyHint(text: String) {
    Column(
        modifier = Modifier.fillMaxWidth().padding(vertical = 48.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            text,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.outline,
            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
        )
    }
}

// =====================================================================
// 新建/编辑对话框
// =====================================================================

private data class TriggerChoice(val label: String, val offsetMinutes: Long?, val custom: Boolean = false)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ReminderEditDialog(
    initial: ReminderRepository.ReminderEntity?,
    allocateId: () -> Int,
    onDismiss: () -> Unit,
    onSave: (ReminderRepository.ReminderEntity, sendNow: Boolean) -> Unit,
) {
    var title by remember { mutableStateOf(initial?.title ?: "") }
    var cmdType by remember { mutableStateOf(initial?.cmdType ?: BleProto.CMD_REMINDER) }
    var triggerIdx by remember { mutableStateOf(
        if (initial?.triggerEpochSec != null) 4 else 0,
    ) }
    var customMs by remember { mutableStateOf(initial?.triggerEpochSec?.times(1000L) ?: 0L) }
    var showDatePicker by remember { mutableStateOf(false) }
    var showTimePicker by remember { mutableStateOf(false) }
    var titleError by remember { mutableStateOf(false) }

    val choices = remember {
        listOf(
            TriggerChoice("即时", 0L),
            TriggerChoice("+5分钟", 5L),
            TriggerChoice("+30分钟", 30L),
            TriggerChoice("+1小时", 60L),
            TriggerChoice("自定义", null, custom = true),
        )
    }

    val titleBytes = title.toByteArray(Charsets.UTF_8).size
    val truncated = titleBytes > BleProto.MAX_TITLE_BYTES

    fun buildTrigger(): Long? = when {
        triggerIdx == 0 -> null
        choices[triggerIdx].custom -> if (customMs > 0) customMs / 1000 else null
        else -> System.currentTimeMillis() / 1000 + (choices[triggerIdx].offsetMinutes ?: 0L) * 60
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (initial == null) "新建提醒" else "编辑提醒 #${initial.id}") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                OutlinedTextField(
                    value = title,
                    onValueChange = { title = it; titleError = false },
                    label = { Text("标题（UTF-8，手表显示）") },
                    isError = titleError || truncated,
                    supportingText = {
                        Text(
                            when {
                                titleError -> "标题不能为空"
                                truncated -> "$titleBytes/${BleProto.MAX_TITLE_BYTES} 字节，超出部分发送时按字符边界截断"
                                else -> "$titleBytes/${BleProto.MAX_TITLE_BYTES} 字节（约 8 个汉字）"
                            },
                            color = if (titleError || truncated) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )

                Spacer(Modifier.height(12.dp))
                Text("类型", style = MaterialTheme.typography.labelLarge)
                Spacer(Modifier.height(6.dp))
                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    SegmentedButton(
                        selected = cmdType == BleProto.CMD_REMINDER,
                        onClick = { cmdType = BleProto.CMD_REMINDER },
                        shape = SegmentedButtonDefaults.itemShape(index = 0, count = 2),
                    ) { Text("提醒") }
                    SegmentedButton(
                        selected = cmdType == BleProto.CMD_NOTIFY,
                        onClick = { cmdType = BleProto.CMD_NOTIFY },
                        shape = SegmentedButtonDefaults.itemShape(index = 1, count = 2),
                    ) { Text("通知") }
                }

                Spacer(Modifier.height(12.dp))
                Text("触发时间", style = MaterialTheme.typography.labelLarge)
                Spacer(Modifier.height(6.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                    choices.forEachIndexed { i, c ->
                        val selected = triggerIdx == i
                        androidx.compose.material3.FilterChip(
                            selected = selected,
                            onClick = {
                                triggerIdx = i
                                if (c.custom && customMs == 0L) showDatePicker = true
                            },
                            label = { Text(c.label, style = MaterialTheme.typography.labelSmall) },
                        )
                    }
                }
                if (choices[triggerIdx].custom) {
                    Spacer(Modifier.height(6.dp))
                    Text(
                        if (customMs > 0) "已选：${com.aiwatch.companion.ui.formatFull(customMs)}"
                        else "未选择具体时间",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    TextButton(onClick = { showDatePicker = true }) { Text("选择日期与时间") }
                }
            }
        },
        confirmButton = {
            Row {
                TextButton(
                    onClick = {
                        if (title.isNotBlank()) {
                            val now = System.currentTimeMillis()
                            val entity = (initial ?: ReminderRepository.ReminderEntity(
                                id = allocateId(),
                                title = "",
                                cmdType = cmdType,
                                flags = BleProto.FLAG_ACTIVE,
                                triggerEpochSec = null,
                                createdAt = now,
                                updatedAt = now,
                            )).copy(
                                title = title.trim(),
                                cmdType = cmdType,
                                triggerEpochSec = buildTrigger(),
                                updatedAt = now,
                            )
                            onSave(entity, true)
                        } else {
                            titleError = true
                        }
                    },
                ) { Text("保存并发送") }
                TextButton(
                    onClick = {
                        if (title.isNotBlank()) {
                            val now = System.currentTimeMillis()
                            val entity = (initial ?: ReminderRepository.ReminderEntity(
                                id = allocateId(),
                                title = "",
                                cmdType = cmdType,
                                flags = BleProto.FLAG_ACTIVE,
                                triggerEpochSec = null,
                                createdAt = now,
                                updatedAt = now,
                            )).copy(
                                title = title.trim(),
                                cmdType = cmdType,
                                triggerEpochSec = buildTrigger(),
                                updatedAt = now,
                            )
                            onSave(entity, false)
                        } else {
                            titleError = true
                        }
                    },
                ) { Text("仅保存") }
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("取消") }
        },
    )

    if (showDatePicker) {
        val dp = rememberDatePickerState(initialSelectedDateMillis = System.currentTimeMillis())
        DatePickerDialog(
            onDismissRequest = { showDatePicker = false },
            confirmButton = {
                TextButton(onClick = {
                    dp.selectedDateMillis?.let {
                        customMs = it
                        showDatePicker = false
                        showTimePicker = true
                    }
                }) { Text("下一步") }
            },
            dismissButton = { TextButton(onClick = { showDatePicker = false }) { Text("取消") } },
        ) {
            DatePicker(state = dp)
        }
    }

    if (showTimePicker) {
        val cal = Calendar.getInstance()
        val tp = rememberTimePickerState(
            initialHour = cal.get(Calendar.HOUR_OF_DAY),
            initialMinute = cal.get(Calendar.MINUTE),
            is24Hour = true,
        )
        AlertDialog(
            onDismissRequest = { showTimePicker = false },
            title = { Text("选择时间") },
            text = { TimePicker(state = tp) },
            confirmButton = {
                TextButton(onClick = {
                    val c = Calendar.getInstance().apply {
                        timeInMillis = if (customMs > 0) customMs else System.currentTimeMillis()
                        set(Calendar.HOUR_OF_DAY, tp.hour)
                        set(Calendar.MINUTE, tp.minute)
                        set(Calendar.SECOND, 0)
                    }
                    customMs = c.timeInMillis
                    showTimePicker = false
                }) { Text("确定") }
            },
            dismissButton = { TextButton(onClick = { showTimePicker = false }) { Text("取消") } },
        )
    }
}
