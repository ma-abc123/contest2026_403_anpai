package com.aiwatch.companion.data

import android.content.Context
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File

/**
 * 提醒/通知本地存储（JSON 文件）：
 * - 提醒列表（id 1..255 循环分配，同 id 重发即覆盖更新）
 * - 发送记录（保留最近 100 条）
 */
class ReminderRepository(context: Context) {

    @Serializable
    data class ReminderEntity(
        val id: Int,
        val title: String,
        val cmdType: Int,             // 1=提醒 2=通知
        val flags: Int,               // bit0=已读 bit1=活跃
        val triggerEpochSec: Long?,   // null=即时
        val createdAt: Long,
        val updatedAt: Long,
        val lastSendState: String? = null,
        val lastSendAt: Long? = null,
    ) {
        val isRead: Boolean get() = (flags and 0x01) != 0
    }

    @Serializable
    data class SendRecord(
        val timeMs: Long,
        val title: String,
        val cmdType: Int,
        val result: String,
        val note: String? = null,
    )

    @Serializable
    private data class Store(
        val reminders: List<ReminderEntity> = emptyList(),
        val records: List<SendRecord> = emptyList(),
        val nextId: Int = 1,
    )

    private val json = Json { prettyPrint = true; ignoreUnknownKeys = true }
    private val file = File(context.filesDir, "reminders.json")

    private val _reminders = MutableStateFlow<List<ReminderEntity>>(emptyList())
    val reminders: StateFlow<List<ReminderEntity>> = _reminders.asStateFlow()

    private val _records = MutableStateFlow<List<SendRecord>>(emptyList())
    val records: StateFlow<List<SendRecord>> = _records.asStateFlow()

    private var nextId = 1

    init {
        val loaded = runCatching {
            if (file.exists()) json.decodeFromString<Store>(file.readText()) else null
        }.getOrNull()
        if (loaded != null) {
            _reminders.value = loaded.reminders
            _records.value = loaded.records
            nextId = loaded.nextId
        }
    }

    private fun save(store: Store = Store(_reminders.value, _records.value, nextId)) {
        runCatching {
            file.writeText(json.encodeToString(Store.serializer(), store))
        }
    }

    /** 分配新 id：1..255 循环，跳过占用 */
    @Synchronized
    fun allocateId(): Int {
        val used = _reminders.value.map { it.id }.toSet()
        var candidate = nextId
        repeat(256) {
            if (candidate > 255) candidate = 1
            if (candidate !in used) {
                nextId = if (candidate >= 255) 1 else candidate + 1
                return candidate
            }
            candidate++
        }
        // 全部占用（几乎不可能）：强占 nextId
        val fallback = nextId
        nextId = if (fallback >= 255) 1 else fallback + 1
        return fallback
    }

    @Synchronized
    fun upsert(entity: ReminderEntity) {
        val list = _reminders.value.toMutableList()
        val idx = list.indexOfFirst { it.id == entity.id }
        if (idx >= 0) list[idx] = entity else list.add(entity)
        _reminders.value = list.sortedBy { it.id }
        save()
    }

    @Synchronized
    fun delete(id: Int) {
        _reminders.value = _reminders.value.filterNot { it.id == id }
        save()
    }

    @Synchronized
    fun updateFlags(id: Int, flags: Int) {
        val list = _reminders.value.map { if (it.id == id) it.copy(flags = flags, updatedAt = System.currentTimeMillis()) else it }
        _reminders.value = list
        save()
    }

    @Synchronized
    fun updateSendResult(id: Int, result: String, atMs: Long) {
        val list = _reminders.value.map { if (it.id == id) it.copy(lastSendState = result, lastSendAt = atMs) else it }
        _reminders.value = list
        save()
    }

    @Synchronized
    fun addRecord(record: SendRecord) {
        val list = (_records.value + record).takeLast(MAX_RECORDS)
        _records.value = list
        save()
    }

    @Synchronized
    fun clearRecords() {
        _records.value = emptyList()
        save()
    }

    companion object {
        const val MAX_RECORDS = 100
    }
}
