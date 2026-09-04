package com.aiwatch.companion.ble

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.TimeZone
import java.util.UUID

/**
 * AI-Watch-403 自定义 GATT 协议编解码。
 *
 * 规范要点：
 * - 所有多字节字段一律小端序（LE）
 * - 所有帧第一个字节为协议版本，当前必须为 0x01
 * - 手表为 Peripheral，手机为 Central，无需配对加密
 */
object BleProto {

    // ---------- GATT UUID ----------
    val SERVICE_UUID: UUID = UUID.fromString("12345678-9abc-def0-1234-56789abcdef0")
    val STATUS_UUID: UUID = UUID.fromString("12345678-9abc-def0-1234-56789abcdef1")
    val TIME_SYNC_UUID: UUID = UUID.fromString("12345678-9abc-def0-1234-56789abcdef2")
    val DATA_UPLOAD_UUID: UUID = UUID.fromString("12345678-9abc-def0-1234-56789abcdef3")
    val COMMAND_UUID: UUID = UUID.fromString("12345678-9abc-def0-1234-56789abcdef4")
    val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    const val PROTOCOL_VERSION = 0x01
    const val DEVICE_NAME_PREFIX = "AI-Watch-403"

    // TimeSync 合法区间（2020-01-01 ~ 2100-01-01 UTC 秒）
    const val TIME_SYNC_MIN_UTC = 1577836800L
    const val TIME_SYNC_MAX_UTC = 4102444800L

    // Command 标题上限（字节，非字符），固定头 9 字节
    const val MAX_TITLE_BYTES = 24

    // AI_TEXT(v2) 回复文本上限：帧最长 9+224=233 字节，需 MTU≥247
    const val AI_TEXT_MAX_BYTES = 224
    const val COMMAND_HEADER_LEN = 9

    // 手表最多存 8 条提醒
    const val WATCH_MAX_REMINDERS = 8

    // AI_TIMER 时长合法区间（秒）
    const val AI_TIMER_MIN_S = 1L
    const val AI_TIMER_MAX_S = 86400L

    // Command cmd_type
    const val CMD_REMINDER = 0x01
    const val CMD_NOTIFY = 0x02
    const val CMD_CLEAR_ALL = 0x03
    const val CMD_AI_TEXT = 0x04
    const val CMD_AI_TIMER = 0x05

    // Command flags
    const val FLAG_READ = 0x01
    const val FLAG_ACTIVE = 0x02

    // DataUpload sensor_type
    const val SENSOR_TEMPERATURE = 0x01
    const val SENSOR_HUMIDITY = 0x02
    const val SENSOR_HEART_RATE = 0x03
    const val SENSOR_SPO2 = 0x04
    const val SENSOR_STEPS = 0x05        // v3 今日步数
    const val SENSOR_ACTIVITY = 0x06     // v3 活动状态
    const val SENSOR_AI_TRIGGER = 0x10
    const val SENSOR_FALL = 0x11         // v3 疑似跌倒
    const val SENSOR_MOTION = 0x12       // v3 原始运动采集流

    // ACTIVITY（0x06）取值
    const val ACTIVITY_REST = 0
    const val ACTIVITY_WALKING = 1
    const val ACTIVITY_RUNNING = 2

    // FALL_EVENT（0x11）event 取值
    const val FALL_EVENT_CONFIRMED = 1   // 疑似跌倒确认（倒计时结束无人取消）
    const val FALL_EVENT_CANCELED = 2    // 跌倒告警已取消
    const val FALL_EVENT_TEST = 3        // 测试事件（联调演练）

    // AI 触发来源（sensor_type=0x10 的 source 字段）
    const val AI_SRC_ASSISTANT_PAGE = 0x01
    const val AI_SRC_HR_PAGE = 0x02

    // AI 手动触发（手机端本地语义，不进协议）
    const val AI_SRC_MANUAL = 0x00

    // ---------- 数据模型 ----------

    /** Status(f1)：[version(1)][conn_state(1)][last_sync(4)] */
    data class WatchStatus(
        val version: Int,
        val connected: Boolean,
        val lastSyncUtc: Long,
    )

    /** DataUpload(f3) 解析结果 */
    data class SensorSample(val sensorType: Int, val value: Float) {
        val known: Boolean get() = sensorType in SENSOR_TEMPERATURE..SENSOR_SPO2
    }

    /**
     * AI 触发（v2，sensor_type=0x10）：[version(1)][0x10(1)][source(1)][context(N)]
     * source=0x01 AI 助手页（context 空）；0x02 心率/血氧页（context=hr(2)+spo2(2)，0=未知）。
     * 未知 source / 任意 context 长度都容忍（不崩溃）。
     */
    data class AiTrigger(
        val source: Int,
        val hr: Int = 0,
        val spo2: Int = 0,
    ) {
        val knownSource: Boolean get() = source == AI_SRC_ASSISTANT_PAGE || source == AI_SRC_HR_PAGE
    }

    /** AI 触发帧解析：帧非法（版本/长度）返回 null；source 未知也返回（由调用方决定忽略） */
    fun parseAiTrigger(bytes: ByteArray): AiTrigger? {
        if (bytes.size < 3) return null
        if (bytes[0].toInt() and 0xFF != PROTOCOL_VERSION) return null
        if (bytes[1].toInt() and 0xFF != SENSOR_AI_TRIGGER) return null
        val source = bytes[2].toInt() and 0xFF
        val ctxLen = bytes.size - 3
        var hr = 0
        var spo2 = 0
        if (source == AI_SRC_HR_PAGE && ctxLen >= 4) {
            hr = readUInt16LE(bytes, 3)
            spo2 = readUInt16LE(bytes, 5)
        }
        return AiTrigger(source, hr, spo2)
    }

    // ---------- v3 运动数据模型 ----------

    /** 今日步数（0x05）：[version(1)][0x05(1)][steps uint32 LE(4)] */
    data class StepsSample(val steps: Long)

    /** 活动状态（0x06）：[version(1)][0x06(1)][activity uint8(1)] */
    data class ActivitySample(val activity: Int)

    /** 疑似跌倒（0x11）：[version(1)][0x11(1)][event(1)][impact_mg uint16 LE(2)][angle_deg(1)][reserved×4] */
    data class FallEvent(
        val event: Int,
        val impactMg: Int,
        val angleDeg: Int,
        val timeMs: Long = System.currentTimeMillis(),
    ) {
        val label: String get() = when (event) {
            FALL_EVENT_CONFIRMED -> "疑似跌倒确认"
            FALL_EVENT_CANCELED -> "跌倒告警已取消"
            FALL_EVENT_TEST -> "跌倒测试事件"
            else -> "未知事件($event)"
        }
    }

    /** 原始运动采样（0x12）：[version(1)][0x12(1)][t_ms uint32 LE(4)][x y z gx gy gz int16 LE×6] */
    data class MotionSample(
        val tMs: Long,
        val x: Int, val y: Int, val z: Int,
        val gx: Int, val gy: Int, val gz: Int,
    )

    /** STEPS 解析：帧非法返回 null */
    fun parseSteps(bytes: ByteArray): StepsSample? {
        if (bytes.size != 6) return null
        if (bytes[0].toInt() and 0xFF != PROTOCOL_VERSION) return null
        if (bytes[1].toInt() and 0xFF != SENSOR_STEPS) return null
        return StepsSample(readUInt32LE(bytes, 2))
    }

    /** ACTIVITY 解析：帧非法返回 null */
    fun parseActivity(bytes: ByteArray): ActivitySample? {
        if (bytes.size != 3) return null
        if (bytes[0].toInt() and 0xFF != PROTOCOL_VERSION) return null
        if (bytes[1].toInt() and 0xFF != SENSOR_ACTIVITY) return null
        return ActivitySample(bytes[2].toInt() and 0xFF)
    }

    /** FALL_EVENT 解析：帧非法返回 null（跌倒事件不允许丢，调用方必须入库并通知） */
    fun parseFallEvent(bytes: ByteArray): FallEvent? {
        if (bytes.size != 10) return null
        if (bytes[0].toInt() and 0xFF != PROTOCOL_VERSION) return null
        if (bytes[1].toInt() and 0xFF != SENSOR_FALL) return null
        val event = bytes[2].toInt() and 0xFF
        val impact = readUInt16LE(bytes, 3)
        val angle = bytes[5].toInt() and 0xFF
        return FallEvent(event, impact, angle)
    }

    /** MOTION_DATA 解析：帧非法返回 null */
    fun parseMotionSample(bytes: ByteArray): MotionSample? {
        if (bytes.size != 16) return null
        if (bytes[0].toInt() and 0xFF != PROTOCOL_VERSION) return null
        if (bytes[1].toInt() and 0xFF != SENSOR_MOTION) return null
        val t = readUInt32LE(bytes, 2)
        return MotionSample(t, readInt16LE(bytes, 4), readInt16LE(bytes, 6), readInt16LE(bytes, 8),
            readInt16LE(bytes, 10), readInt16LE(bytes, 12), readInt16LE(bytes, 14))
    }

    // ---------- 帧构造 ----------

    /** 当前时区偏移（分钟，含 DST），有符号 */
    fun currentTzOffsetMinutes(): Int =
        TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000

    /**
     * TimeSync(f2)：[version=1(1)][utc(4,LE)][tz_offset_minutes(2,LE 有符号)]
     * 越界返回 null（手表会拒绝非法帧）。
     */
    fun buildTimeSync(
        nowMillis: Long = System.currentTimeMillis(),
        tzOffsetMinutes: Int = currentTzOffsetMinutes(),
    ): ByteArray? {
        val utc = nowMillis / 1000L
        if (utc !in TIME_SYNC_MIN_UTC..TIME_SYNC_MAX_UTC) return null
        if (tzOffsetMinutes !in -720..840) return null
        return ByteBuffer.allocate(7)
            .order(ByteOrder.LITTLE_ENDIAN)
            .put(PROTOCOL_VERSION.toByte())
            .putInt(utc.toInt())
            .putShort(tzOffsetMinutes.toShort())
            .array()
    }

    /**
     * Command(f4)：[version(1)][cmd_type(1)][id(1)][flags(1)][timestamp(4,LE)][title_len(1)][title(N)]
     * 总长 = 9 + title_len，标题按 UTF-8 字符边界截断。
     * @param maxTitleBytes 标题字节上限：常规 24；AI_TEXT(0x04) 为 224
     * @param timestampSec 常规为触发时刻 UTC 秒；AI_TIMER(0x05) 时为时长秒（1..86400）
     */
    fun buildCommand(
        cmdType: Int,
        id: Int,
        flags: Int,
        timestampSec: Long,
        title: String,
        maxTitleBytes: Int = MAX_TITLE_BYTES,
    ): ByteArray {
        val safeTitle = truncateTitle(title.trim(), maxTitleBytes)
        val titleBytes = safeTitle.toByteArray(Charsets.UTF_8)
        return ByteBuffer.allocate(COMMAND_HEADER_LEN + titleBytes.size)
            .order(ByteOrder.LITTLE_ENDIAN)
            .put(PROTOCOL_VERSION.toByte())
            .put(cmdType.toByte())
            .put(id.coerceIn(0, 255).toByte())
            .put(flags.coerceIn(0, 255).toByte())
            .putInt(timestampSec.toInt())
            .put(titleBytes.size.toByte())
            .put(titleBytes)
            .array()
    }

    /** 清除全部（cmd_type=0x03）：固定 9 字节，title_len=0，其余字段忽略 */
    fun buildClearAll(): ByteArray =
        ByteBuffer.allocate(COMMAND_HEADER_LEN)
            .order(ByteOrder.LITTLE_ENDIAN)
            .put(PROTOCOL_VERSION.toByte())
            .put(CMD_CLEAR_ALL.toByte())
            .put(0)
            .put(0)
            .putInt(0)
            .put(0)
            .array()

    /** 标题截断：落在 UTF-8 字符边界上（中文 3 字节/字），默认 24 字节（约 8 个汉字） */
    fun truncateTitle(title: String, maxBytes: Int = MAX_TITLE_BYTES): String {
        if (title.toByteArray(Charsets.UTF_8).size <= maxBytes) return title
        val sb = StringBuilder()
        var used = 0
        for (ch in title) {
            val len = when {
                ch.code <= 0x7F -> 1
                ch.code <= 0x7FF -> 2
                ch.code <= 0xFFFF -> 3
                else -> 4
            }
            if (used + len > maxBytes) break
            sb.append(ch)
            used += len
        }
        return sb.toString()
    }

    // ---------- 帧解析 ----------

    /** Status(f1) 解析，非法返回 null */
    fun parseStatus(bytes: ByteArray): WatchStatus? {
        if (bytes.size != 6) return null
        val version = bytes[0].toInt() and 0xFF
        if (version != PROTOCOL_VERSION) return null
        val connected = (bytes[1].toInt() and 0xFF) == 1
        val lastSync = readUInt32LE(bytes, 2)
        return WatchStatus(version, connected, lastSync)
    }

    /**
     * DataUpload(f3) 解析：[version(1)][sensor_type(1)][payload(N)]
     * 未知 sensor_type 返回 known=false 的样本（调用方记录后忽略，不崩溃）；
     * 帧非法（版本/长度）返回 null。
     */
    fun parseDataUpload(bytes: ByteArray): SensorSample? {
        if (bytes.size < 2) return null
        if (bytes[0].toInt() and 0xFF != PROTOCOL_VERSION) return null
        val sensorType = bytes[1].toInt() and 0xFF
        return when (sensorType) {
            SENSOR_TEMPERATURE, SENSOR_HUMIDITY -> {
                if (bytes.size < 6) null
                else SensorSample(sensorType, Float.fromBits(readUInt32LE(bytes, 2).toInt()))
            }
            SENSOR_HEART_RATE, SENSOR_SPO2 -> {
                if (bytes.size < 4) null
                else SensorSample(sensorType, readUInt16LE(bytes, 2).toFloat())
            }
            else -> SensorSample(sensorType, Float.NaN)
        }
    }

    // ---------- 错误码 ----------

    /** GATT/ATT 错误码 → 可读文案 */
    fun gattStatusMessage(status: Int): String = when (status) {
        0 -> "成功"
        0x01 -> "无效句柄 (0x01)"
        0x02 -> "读不支持 (0x02)"
        0x03 -> "写不支持 (0x03)"
        0x05 -> "认证不足 (0x05)"
        0x07 -> "偏移非法 (0x07)"
        0x08 -> "连接超时 (0x08)"
        0x0D -> "长度非法 (0x0D)"
        0x0F -> "属性不支持 (0x0F)"
        0x11 -> "手表队列已满 (0x11 Insufficient Resources)"
        0x13 -> "值非法/版本或越界 (0x13)"
        0x1F -> "操作排队中 (0x1F)"
        0x85, 133 -> "GATT 错误 133/0x85（常见于链路断开/设备不可达）"
        0x86 -> "协议错误 (0x86)"
        0x87 -> "加密不足 (0x87)"
        0x89 -> "连接即将断开 (0x89)"
        257 -> "内部错误 (257)"
        else -> "错误码 0x%02X (%d)".format(status, status)
    }

    // ---------- 工具 ----------

    fun readUInt16LE(b: ByteArray, off: Int): Int =
        (b[off].toInt() and 0xFF) or ((b[off + 1].toInt() and 0xFF) shl 8)

    /** 有符号 int16 LE（用于加速度/陀螺仪，mg/mdps 可负） */
    fun readInt16LE(b: ByteArray, off: Int): Int =
        (b[off].toInt() and 0xFF) or (b[off + 1].toInt() shl 8)

    fun readUInt32LE(b: ByteArray, off: Int): Long =
        ((b[off].toLong() and 0xFF)) or
            ((b[off + 1].toLong() and 0xFF) shl 8) or
            ((b[off + 2].toLong() and 0xFF) shl 16) or
            ((b[off + 3].toLong() and 0xFF) shl 24)

    fun toHex(bytes: ByteArray): String =
        bytes.joinToString(" ") { "%02X".format(it) }

    fun sensorName(type: Int): String = when (type) {
        SENSOR_TEMPERATURE -> "温度"
        SENSOR_HUMIDITY -> "湿度"
        SENSOR_HEART_RATE -> "心率"
        SENSOR_SPO2 -> "血氧"
        SENSOR_STEPS -> "步数"
        SENSOR_ACTIVITY -> "活动状态"
        SENSOR_FALL -> "疑似跌倒"
        SENSOR_MOTION -> "原始运动"
        else -> "未知(0x%02X)".format(type)
    }

    /** 活动状态可读名（0x06） */
    fun activityName(activity: Int): String = when (activity) {
        ACTIVITY_REST -> "静止"
        ACTIVITY_WALKING -> "步行"
        ACTIVITY_RUNNING -> "跑步"
        else -> "未知(%d)".format(activity)
    }
}
