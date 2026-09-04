package com.aiwatch.companion.data

import android.content.Context
import com.aiwatch.companion.ble.BleProto
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelChildren
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.File
import java.io.PrintWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 原始运动流存档（v3，sensor_type=0x12）：
 * 每帧一行 JSON 追加写入（JSON Lines），文件头/尾各一条 meta 行记录起止与帧数，
 * 供电脑端离线阈值分析。默认约 8Hz（手表侧 4 条转发 1 条），BLE 回调线程仅入队，
 * 由独立 IO 协程消费写盘，不阻塞通知确认。
 */
class MotionRecorder(private val context: Context) {

    private var channel = Channel<BleProto.MotionSample>(capacity = 512)
    private val ioScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    private val _recording = MutableStateFlow(false)
    val recording: StateFlow<Boolean> = _recording.asStateFlow()

    private var writer: PrintWriter? = null
    private var file: File? = null
    private var count = 0

    /** 当前存档文件路径（未录制为 null），UI 展示用 */
    val currentPath: String? get() = file?.absolutePath

    val currentCount: Int get() = count

    /** 开始录制：新建 JSONL 文件并写入 meta 头（重复调用幂等） */
    fun start() {
        if (_recording.value) return
        channel = Channel(512) // 重建通道（上次 stop 时已 close）
        runCatching {
            val dir = File(context.filesDir, "motion")
            dir.mkdirs()
            val name = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
            file = File(dir, "motion_$name.jsonl")
            writer = file?.writer(Charsets.UTF_8)?.buffered()?.let { PrintWriter(it) }
            count = 0
            writer?.println("{\"meta\":\"start\",\"start_ms\":${System.currentTimeMillis()}}")
            _recording.value = true
        }.onFailure {
            _recording.value = false
        }
    }

    /** BLE 回调线程调用：非阻塞入队 */
    fun append(sample: BleProto.MotionSample) {
        if (!_recording.value) return
        runCatching { channel.trySend(sample) }
    }

    /** 结束录制：关闭队列，写完剩余帧后落盘 */
    fun stop() {
        if (!_recording.value) return
        val ch = channel
        ch.close()
        ioScope.launch {
            for (s in ch) writeLine(s)
            runCatching {
                writer?.println("{\"meta\":\"end\",\"count\":$count}")
                writer?.flush()
                writer?.close()
            }
            writer = null
            _recording.value = false
        }
    }

    private fun writeLine(s: BleProto.MotionSample) {
        writer?.println(
            "{\"t\":${s.tMs},\"x\":${s.x},\"y\":${s.y},\"z\":${s.z},\"gx\":${s.gx},\"gy\":${s.gy},\"gz\":${s.gz}}",
        )
        count++
    }

    fun shutdown() {
        ioScope.coroutineContext.cancelChildren()
    }
}
