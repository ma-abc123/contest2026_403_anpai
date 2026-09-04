package com.aiwatch.companion.ble

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

enum class LogDir { TX, RX, EVENT, ERROR }

/**
 * 联调日志：原始帧 hex + 事件，环形缓冲（默认 500 条）。
 */
data class LogEntry(
    val timeMs: Long,
    val dir: LogDir,
    val tag: String,
    val hex: String?,
    val note: String,
)

class WatchLog(private val capacity: Int = 500) {

    private val lock = Any()
    private val buffer = ArrayDeque<LogEntry>(capacity + 8)

    private val _entries = MutableStateFlow<List<LogEntry>>(emptyList())
    val entries: StateFlow<List<LogEntry>> = _entries.asStateFlow()

    val size: Int get() = synchronized(lock) { buffer.size }

    fun tx(tag: String, bytes: ByteArray, note: String) =
        add(LogEntry(now(), LogDir.TX, tag, BleProto.toHex(bytes), note))

    fun rx(tag: String, bytes: ByteArray, note: String) =
        add(LogEntry(now(), LogDir.RX, tag, BleProto.toHex(bytes), note))

    fun event(text: String) = add(LogEntry(now(), LogDir.EVENT, "", null, text))

    fun error(text: String) = add(LogEntry(now(), LogDir.ERROR, "", null, text))

    fun clear() {
        synchronized(lock) { buffer.clear() }
        publish()
    }

    fun copyAllText(): String = synchronized(lock) {
        buffer.joinToString("\n") { e ->
            buildString {
                append(formatTime(e.timeMs))
                append(' ')
                append(when (e.dir) {
                    LogDir.TX -> "↑TX"
                    LogDir.RX -> "↓RX"
                    LogDir.EVENT -> " · "
                    LogDir.ERROR -> " ✗ "
                })
                if (e.tag.isNotEmpty()) append(' ').append(e.tag)
                if (e.hex != null) append(" [").append(e.hex).append(']')
                if (e.note.isNotEmpty()) append(' ').append(e.note)
            }
        }
    }

    private fun add(e: LogEntry) {
        synchronized(lock) {
            buffer.addLast(e)
            while (buffer.size > capacity) buffer.removeFirst()
        }
        publish()
    }

    private fun publish() {
        _entries.value = synchronized(lock) { buffer.toList() }
    }

    private fun now() = System.currentTimeMillis()

    companion object {
        private val SDF = java.text.SimpleDateFormat("HH:mm:ss.SSS", java.util.Locale.US)
        fun formatTime(ms: Long): String = synchronized(SDF) { SDF.format(java.util.Date(ms)) }
    }
}
