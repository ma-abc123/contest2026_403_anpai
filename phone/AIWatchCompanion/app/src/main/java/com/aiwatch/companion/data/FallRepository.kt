package com.aiwatch.companion.data

import android.content.Context
import com.aiwatch.companion.ble.BleProto
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.serialization.Serializable
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json
import java.io.File

/** 一条跌倒事件记录（v3，本地持久化） */
@Serializable
data class FallRecord(
    val event: Int,
    val impactMg: Int,
    val angleDeg: Int,
    val timeMs: Long,
) {
    /** 文案统一用"疑似跌倒"，不得表述为医疗级判断 */
    val label: String get() = when (event) {
        BleProto.FALL_EVENT_CONFIRMED -> "疑似跌倒确认"
        BleProto.FALL_EVENT_CANCELED -> "跌倒告警已取消"
        BleProto.FALL_EVENT_TEST -> "跌倒测试事件"
        else -> "未知事件($event)"
    }
}

/**
 * 跌倒事件仓库（JSON 文件，保留最近 100 条）。
 * 跌倒事件不做频控、不允许丢：收到即入库。
 */
class FallRepository(private val context: Context) {

    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }
    private val file = File(context.filesDir, "fall_events.json")

    private val _events = MutableStateFlow<List<FallRecord>>(emptyList())
    val events: StateFlow<List<FallRecord>> = _events.asStateFlow()

    /** 最近一条（UI 置顶告警用） */
    val latest: FallRecord? get() = _events.value.firstOrNull()

    init {
        _events.value = runCatching {
            if (file.exists()) json.decodeFromString<List<FallRecord>>(file.readText()) else emptyList()
        }.getOrDefault(emptyList())
    }

    /** 入库（新的在前），并持久化 */
    fun add(rec: FallRecord) {
        _events.value = (listOf(rec) + _events.value).take(MAX)
        persist()
    }

    fun clear() {
        _events.value = emptyList()
        persist()
    }

    private fun persist() {
        runCatching {
            file.writeText(json.encodeToString(ListSerializer(FallRecord.serializer()), _events.value))
        }
    }

    companion object {
        const val MAX = 100
    }
}
