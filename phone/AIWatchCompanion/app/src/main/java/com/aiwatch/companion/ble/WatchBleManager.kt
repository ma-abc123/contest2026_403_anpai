package com.aiwatch.companion.ble

import android.Manifest
import android.annotation.SuppressLint
import android.app.Application
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanRecord
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.SystemClock
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID
import java.util.concurrent.atomic.AtomicInteger

/** 连接阶段 */
enum class Phase { IDLE, SCANNING, CONNECTING, INITIALIZING, CONNECTED, RECONNECTING, BLUETOOTH_OFF }

/** 单个传感器读数（值 + 收到时刻） */
data class SensorValue(val value: Float, val atMs: Long)

/** 仪表盘快照：缺数据为 null（UI 显示 --） */
data class SensorSnapshot(
    val temperature: SensorValue? = null,
    val humidity: SensorValue? = null,
    val heartRate: SensorValue? = null,
    val spo2: SensorValue? = null,
)

/** 扫描命中的设备 */
data class ScanHit(val name: String?, val address: String, val rssi: Int)

/** 对外暴露的完整状态 */
data class WatchState(
    val phase: Phase = Phase.IDLE,
    val deviceName: String? = null,
    val deviceAddress: String? = null,
    val mtu: Int = 23,
    val lastSyncUtc: Long = 0L,
    val retryAttempt: Int = 0,
    val nextRetryAtMs: Long = 0L,
    val sensors: SensorSnapshot = SensorSnapshot(),
    val scanHits: List<ScanHit> = emptyList(),
    val queueSize: Int = 0,
    // v3 运动数据
    val steps: Long = 0L,                          // 今日步数（0x05，手表重启清零）
    val activity: Int = BleProto.ACTIVITY_REST,    // 活动状态（0x06）
    val fallEvent: BleProto.FallEvent? = null,     // 最近一条跌倒事件（0x11）
    val motionCount: Int = 0,                      // 已接收原始运动帧数（0x12）
)

/**
 * 手表连接管理器（应用级单例）：
 * 扫描(名称前缀/128 位服务UUID) → 连接(30s超时) → MTU≥247 → 订阅 f1/f3 → 读 Status → TimeSync → 就绪
 * 断线自动重连（指数退避 2s→4s→8s→15s→30s 封顶，持续重试）；用户主动断开不重连；
 * 蓝牙开关重开 / 应用回前台立即重连；时区变化补发校时；8 小时周期兜底校时。
 * 所有 GATT 写操作经串行队列，0x11 队列满按 2s 退避重试。
 *
 * v3.0.1 重连修复（缺陷报告 2026-10-27）：
 * - 不再用缓存 BluetoothDevice 盲连：手表断连后要 10~40s 才恢复广播，盲连必失败；
 *   重连一律重新扫描，用 ScanResult.device 连接。
 * - 连接超时 15s→30s（对齐 Android 自身超时），放弃时先 disconnect、400ms 后再 close，
 *   避免在 pending connect 上 close 造成协议栈残留（后续连回 status=133 且不自愈）。
 * - 断开即清空 device / statusChar / dataUploadChar / 各类挂起结果。
 * - 连续 2 次 status=133 → 冷却 1s 后重新扫描，不再原地重试。
 * - 用户点击连接不清空退避阶梯（只有从"用户主动断开"恢复时才清零）。
 */
@SuppressLint("MissingPermission")
class WatchBleManager(
    private val context: Application,
    val log: WatchLog,
) {
    companion object {
        const val CONNECT_TIMEOUT_MS = 30_000L      // v3.0.1：对齐 Android connectGatt 自身超时（原 15s 早退会留下半开连接）
        const val STEP_TIMEOUT_MS = 12_000L         // 初始化单步超时
        const val BASE_RETRY_MS = 2_000L            // §2 指数退避 2s 起
        const val MAX_RETRY_MS = 30_000L            // 封顶 30s
        const val GATT_CLOSE_GRACE_MS = 400L        // v3.0.1：disconnect 后等 close 完成，再发起下一次连接
        const val USER_CLOSE_GRACE_MS = 3_000L      // v3.0.1：用户主动断开时等 DISCONNECTED 回调，3s 兜底 close
        const val RECOVERY_COOLDOWN_MS = 1_000L     // v3.0.1：连续 133 后的冷却
        const val FOREGROUND_CHECK_MIN_MS = 10_000L // v3.0.1：回前台链路校验最小间隔
        const val PERIODIC_SYNC_MS = 8L * 3600_000L // §3.2 建议 6~12h 兜底，取 8h
        const val SCAN_WINDOW_MS = 30_000L          // v3.0.1：覆盖手表断连后 10~40s 的广播恢复窗口
        const val WRITE_TIMEOUT_MS = 10_000L        // 单次写等待
        const val ENQUEUE_TIMEOUT_MS = 90_000L      // 入队结果等待（容忍重连）
        const val REQUEST_MTU = 247                 // §2 要求 ≥247
        const val MOTION_LOG_EVERY = 25             // v3：0x12 高频帧日志节流（8Hz 下约 3s 一条）

        /** v3.0.1：重连退避阶梯 2s→4s→8s→15s→30s（与《手机端BLE重连退避需求-2026-10-24》一致） */
        private val RETRY_LADDER_MS = longArrayOf(2_000L, 4_000L, 8_000L, 15_000L, 30_000L)
    }

    // v3：motion 帧日志节流计数
    private var motionLogTick = 0

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    private val _state = MutableStateFlow(WatchState())
    val state: StateFlow<WatchState> = _state.asStateFlow()

    private val _events = MutableSharedFlow<String>(extraBufferCapacity = 16)
    val events: SharedFlow<String> = _events.asSharedFlow()

    /** UI 层发射提示消息（Snackbar） */
    suspend fun emitEvent(text: String) = _events.emit(text)

    /** UI 层提示（非挂起场景：tryEmit，缓冲满时丢弃） */
    fun notifyUser(text: String) {
        _events.tryEmit(text)
    }

    /**
     * AI 触发回调（v2：收到 f3 sensor_type=0x10）。
     * 由 AiSessionManager 注册；回调发生在主线程。
     */
    @Volatile
    var aiTriggerListener: ((source: Int, hr: Int, spo2: Int) -> Unit)? = null

    /** v3：跌倒事件回调（0x11，WatchApp 注册：入库 + 通知；主线程，不允许丢） */
    @Volatile
    var fallListener: ((BleProto.FallEvent) -> Unit)? = null

    /** v3：原始运动采样回调（0x12，WatchApp 注册：MotionRecorder 入队；主线程） */
    @Volatile
    var motionListener: ((BleProto.MotionSample) -> Unit)? = null

    /** 前台服务保活开关（设置页，通过 setKeepAlive 修改） */
    @Volatile
    private var keepAlive = false

    // ---------- 内部状态 ----------
    private var gatt: BluetoothGatt? = null
    private var device: BluetoothDevice? = null
    private var userDisconnected = true
    private val retryAttempt = AtomicInteger(0)
    private val pendingCount = AtomicInteger(0)

    // v3.0.1：连接耗时统计 / 133 连击计数 / 回前台校验节流
    @Volatile
    private var connectStartedAtMs = 0L
    private var lastForegroundCheckMs = 0L
    private val gatt133Count = AtomicInteger(0)

    private var statusChar: BluetoothGattCharacteristic? = null
    private var dataUploadChar: BluetoothGattCharacteristic? = null

    private var scanCallback: ScanCallback? = null
    private var scanJob: Job? = null
    private var reconnectJob: Job? = null
    private var stepTimeoutJob: Job? = null
    private var periodicSyncJob: Job? = null

    /** GATT 就绪信号（订阅完成即置位，先于 phase=CONNECTED，用于 TimeSync 队列放行） */
    private val readySignal = MutableStateFlow(false)

    private data class QueuedWrite(
        val uuid: UUID,
        val bytes: ByteArray,
        val tag: String,
        val note: String,
        val deferred: CompletableDeferred<Int>,
        var retry: Int = 0,
    )

    private val writeQueue = Channel<QueuedWrite>(Channel.UNLIMITED)
    private val writeResults = mutableMapOf<UUID, CompletableDeferred<Int>>()
    private val descriptorResults = mutableMapOf<UUID, CompletableDeferred<Int>>()
    private val readResults = mutableMapOf<UUID, CompletableDeferred<Pair<ByteArray, Int>?>>()

    private val adapter: BluetoothAdapter?
        get() = context.getSystemService(BluetoothManager::class.java)?.adapter

    /** 广播接收：蓝牙开关 / 系统时间与时区变化（须声明在 init 之前） */
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context?, intent: Intent?) {
            when (intent?.action) {
                BluetoothAdapter.ACTION_STATE_CHANGED -> {
                    val s = intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1)
                    scope.launch {
                        when (s) {
                            BluetoothAdapter.STATE_OFF, BluetoothAdapter.STATE_TURNING_OFF -> {
                                log.event("蓝牙已关闭")
                                reconnectJob?.cancel(); reconnectJob = null
                                stopScanInternal()
                                failPendingOps()
                                releaseGatt(0L)
                                device = null
                                statusChar = null
                                dataUploadChar = null
                                gatt133Count.set(0)
                                periodicSyncJob?.cancel(); periodicSyncJob = null
                                // 手表侧蓝牙开关关闭导致设备消失：视为可恢复事件
                                _state.update {
                                    it.copy(
                                        phase = if (userDisconnected) Phase.IDLE else Phase.BLUETOOTH_OFF,
                                        retryAttempt = 0,
                                    )
                                }
                                if (!userDisconnected) log.event("等待蓝牙重新开启后自动重连")
                                updateService()
                            }
                            BluetoothAdapter.STATE_ON -> {
                                log.event("蓝牙已开启")
                                if (!userDisconnected) {
                                    retryAttempt.set(0)
                                    attemptReconnect() // 立即触发一轮重连
                                } else {
                                    _state.update { it.copy(phase = Phase.IDLE) }
                                }
                            }
                        }
                    }
                }
                Intent.ACTION_TIMEZONE_CHANGED, Intent.ACTION_TIME_CHANGED -> {
                    scope.launch {
                        if (_state.value.phase == Phase.CONNECTED) {
                            log.event("系统时间/时区变化 → 补发 TimeSync")
                            enqueueTimeSync("时区/时间变化补发")
                        }
                    }
                }
            }
        }
    }

    init {
        // 广播：蓝牙开关、系统时间/时区变化
        val filter = IntentFilter().apply {
            addAction(BluetoothAdapter.ACTION_STATE_CHANGED)
            addAction(Intent.ACTION_TIME_CHANGED)
            addAction(Intent.ACTION_TIMEZONE_CHANGED)
        }
        ContextCompat.registerReceiver(context, receiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED)

        // 串行写队列消费者（TimeSync / Command 全部走这里）
        scope.launch {
            for (w in writeQueue) {
                readySignal.first { it }
                processWrite(w)
            }
        }
        log.event("WatchBleManager 初始化，等待连接指令")
    }

    // =====================================================================
    // 公开操作
    // =====================================================================

    /**
     * 用户点击「扫描并连接」/ 应用启动恢复。
     *
     * v3.0.1：一律重新扫描，不再用缓存 device 盲连——手表断连后需要 10~40s 才恢复广播，
     * 盲连正好落在空窗里，必然失败并留下半开 GATT client。
     * 仅当从「用户主动断开」态恢复时清零退避阶梯；连续失败期间反复点击保留计数（避免"越点越连不上"）。
     */
    fun scanAndConnect() {
        if (!hasBlePermissions()) {
            _events.tryEmit("缺少蓝牙权限，无法扫描")
            return
        }
        val ad = adapter
        if (ad == null || !ad.isEnabled) {
            _state.update { it.copy(phase = Phase.BLUETOOTH_OFF) }
            _events.tryEmit("蓝牙未开启，请在系统设置中打开")
            log.error("蓝牙适配器不可用或未开启")
            return
        }
        if (_state.value.phase == Phase.CONNECTED) {
            _events.tryEmit("已连接 ${_state.value.deviceName ?: BleProto.DEVICE_NAME_PREFIX}")
            return
        }
        val resumeFromIdle = userDisconnected
        userDisconnected = false
        if (resumeFromIdle) retryAttempt.set(0)     // 新会话清零；重连点击不清零
        reconnectJob?.cancel()
        reconnectJob = null
        stopScanInternal()
        failPendingOps()
        releaseGatt(0L)                             // 摘掉可能残留的半开连接
        device = null                               // 关键：不再盲连缓存地址
        log.event("收到连接请求 → 重新扫描（退避计数 ${retryAttempt.get()}）")
        beginScan()
        updateService()
    }

    /** 用户主动断开：不自动重连 */
    fun disconnectByUser() {
        userDisconnected = true
        reconnectJob?.cancel(); reconnectJob = null
        scanJob?.cancel(); scanJob = null
        stopScanInternal()
        failPendingOps()
        // v3.0.1：只 disconnect，等 DISCONNECTED 回调后再 close；3s 兜底强制关闭
        releaseGatt(USER_CLOSE_GRACE_MS)
        device = null
        statusChar = null
        dataUploadChar = null
        gatt133Count.set(0)
        periodicSyncJob?.cancel(); periodicSyncJob = null
        _state.update { it.copy(phase = Phase.IDLE, retryAttempt = 0, nextRetryAtMs = 0L) }
        log.event("用户主动断开，停止自动重连")
        _events.tryEmit("已断开")
        updateService()
    }

    /** UI 点击扫描列表中的设备 */
    fun connectToAddress(address: String) {
        val ad = adapter ?: return
        if (!hasBlePermissions()) return
        val dev = try {
            ad.getRemoteDevice(address)
        } catch (_: IllegalArgumentException) {
            null
        } ?: return
        userDisconnected = false
        retryAttempt.set(0)
        reconnectJob?.cancel(); reconnectJob = null
        stopScanInternal()
        device = dev
        connectDirect(dev)
    }

    /** 手动「同步时间」 */
    suspend fun syncTimeNow(): Boolean {
        if (_state.value.phase != Phase.CONNECTED) {
            _events.tryEmit("未连接手表")
            return false
        }
        return enqueueTimeSync("手动校时")
    }

    /**
     * 发送提醒/通知/更新（同 id 重发即覆盖更新）。
     * @param timestampSec 常规为触发时刻 UTC 秒；AI_TIMER(0x05) 为时长秒
     * @param maxTitleBytes 标题字节上限（常规 24，AI_TEXT 224）
     * @return 0 成功；负数为本地错误；正数为 ATT/GATT 错误码
     */
    suspend fun sendCommand(
        cmdType: Int,
        id: Int,
        flags: Int,
        timestampSec: Long,
        title: String,
        maxTitleBytes: Int = BleProto.MAX_TITLE_BYTES,
    ): Int {
        if (id !in 1..255) {
            log.error("id 必须为 1..255（0 为手表空槽标记）")
            return -1000
        }
        val frame = BleProto.buildCommand(cmdType, id, flags, timestampSec, title, maxTitleBytes)
        val typeText = when (cmdType) {
            BleProto.CMD_REMINDER -> "提醒"
            BleProto.CMD_NOTIFY -> "通知"
            BleProto.CMD_CLEAR_ALL -> "清空"
            BleProto.CMD_AI_TEXT -> "AI_TEXT"
            BleProto.CMD_AI_TIMER -> "AI_TIMER"
            else -> "cmd=0x%02X".format(cmdType)
        }
        val timeText = when (cmdType) {
            BleProto.CMD_AI_TIMER -> "时长=${timestampSec}s"
            else -> "时间=${formatUtc(timestampSec)}"
        }
        return enqueueWrite(
            BleProto.COMMAND_UUID, frame, "Command(f4)",
            "$typeText id=$id flags=0x%02X".format(flags) +
                " $timeText 标题=${BleProto.truncateTitle(title.trim(), maxTitleBytes)}",
        )
    }

    /** 清除手表全部提醒（cmd_type=0x03，9 字节） */
    suspend fun clearAllOnWatch(): Int {
        if (_state.value.phase != Phase.CONNECTED) {
            _events.tryEmit("未连接手表")
            return -1
        }
        return enqueueWrite(BleProto.COMMAND_UUID, BleProto.buildClearAll(), "Command(f4)", "清除手表全部提醒")
    }

    /**
     * 应用回到前台（v3.0.1 重写）：
     * - CONNECTED：读一次 Status 校验链路——手机侧可能漏收 DISCONNECTED 回调而长期"假在线"，读失败即重连；
     * - RECONNECTING / BLUETOOTH_OFF / IDLE（非用户断开）：立即发起一轮，不等退避；
     * - SCANNING / CONNECTING / INITIALIZING：连接正在进行，保持不动，避免打断。
     */
    fun onAppForeground() {
        if (userDisconnected) return
        val ad = adapter
        if (ad == null || !ad.isEnabled) return
        val now = SystemClock.elapsedRealtime()
        when (_state.value.phase) {
            Phase.CONNECTED -> {
                if (now - lastForegroundCheckMs < FOREGROUND_CHECK_MIN_MS) return
                lastForegroundCheckMs = now
                scope.launch {
                    val pair = readCharacteristic(statusChar)
                    if (pair == null || pair.second != 0) {
                        val why = pair?.let { "status=0x%02X".format(it.second) } ?: "无响应"
                        log.error("回前台链路校验失败（$why）→ 重新扫描连接")
                        handleDisconnected(0x08)
                    } else {
                        log.event("回前台链路校验通过")
                    }
                }
            }
            Phase.RECONNECTING, Phase.BLUETOOTH_OFF, Phase.IDLE -> {
                log.event("应用回前台，立即发起重连（当前 ${_state.value.phase}）")
                reconnectJob?.cancel(); reconnectJob = null
                retryAttempt.set(0)
                attemptReconnect()
            }
            else -> log.event("应用回前台，连接进行中（${_state.value.phase}），保持")
        }
    }

    /** 设置页开关前台服务保活 */
    fun setKeepAlive(enabled: Boolean) {
        keepAlive = enabled
        if (enabled) {
            if (!hasBlePermissions()) {
                log.error("缺少蓝牙权限，未启动前台服务")
                _events.tryEmit("缺少蓝牙权限，无法开启保活")
                return
            }
            try {
                ContextCompat.startForegroundService(context, Intent(context, WatchForegroundService::class.java))
                log.event("前台服务保活已开启")
            } catch (e: Exception) {
                log.error("启动前台服务失败: ${e.message}")
            }
        } else {
            context.stopService(Intent(context, WatchForegroundService::class.java))
            log.event("前台服务保活已关闭")
        }
        updateService()
    }

    fun hasBlePermissions(): Boolean {
        fun has(p: String) =
            ContextCompat.checkSelfPermission(context, p) == PackageManager.PERMISSION_GRANTED
        return if (Build.VERSION.SDK_INT >= 31) {
            has(Manifest.permission.BLUETOOTH_SCAN) && has(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            has(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }

    // =====================================================================
    // 扫描
    // =====================================================================

    private fun beginScan() {
        val ad = adapter
        if (ad == null || !ad.isEnabled) {
            _state.update { it.copy(phase = Phase.BLUETOOTH_OFF) }
            return
        }
        if (!hasBlePermissions()) return
        stopScanInternal()
        _state.update {
            it.copy(phase = Phase.SCANNING, scanHits = emptyList(), retryAttempt = retryAttempt.get())
        }
        log.event("开始扫描（${BleProto.DEVICE_NAME_PREFIX} 前缀 / 128 位服务 UUID，第 ${retryAttempt.get() + 1} 轮）")

        val scanner = try {
            ad.bluetoothLeScanner
        } catch (e: Exception) {
            log.error("获取扫描器失败: ${e.message}")
            scheduleReconnect()
            return
        } ?: run {
            log.error("蓝牙扫描器不可用")
            scheduleReconnect()
            return
        }

        val hits = LinkedHashMap<String, ScanHit>()

        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val dev = result.device ?: return
                // v3.0.1：取名字可能抛 SecurityException（权限被撤销时），不能让扫描回调崩掉
                val name = runCatching { dev.name }.getOrNull()
                val nameMatch = name?.startsWith(BleProto.DEVICE_NAME_PREFIX) == true
                val uuidMatch = scanRecordHasService(result.scanRecord)
                if (!uuidMatch && !nameMatch) return
                val addr = dev.address
                val firstHit = !hits.containsKey(addr)
                hits[addr] = ScanHit(name, addr, result.rssi)
                _state.update { s -> s.copy(scanHits = hits.values.toList()) }
                if (firstHit) {
                    log.event("发现设备 ${name ?: addr} (RSSI ${result.rssi})")
                }
                // 命中即连（首台自动连接；其余展示在列表中供手动选择）
                if (firstHit && _state.value.phase == Phase.SCANNING) {
                    device = dev
                    stopScanInternal()
                    scope.launch { connectDirect(dev) }
                }
            }

            override fun onScanFailed(errorCode: Int) {
                log.error("扫描失败 code=$errorCode")
                scope.launch {
                    stopScanInternal()
                    if (!userDisconnected) {
                        scheduleReconnect()
                    } else {
                        _state.update { it.copy(phase = Phase.IDLE) }
                    }
                }
            }
        }
        scanCallback = cb
        try {
            scanner.startScan(
                null,
                ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),
                cb,
            )
        } catch (e: Exception) {
            log.error("启动扫描失败: ${e.message}")
            scanCallback = null
            scheduleReconnect()
            return
        }

        scanJob = scope.launch {
            delay(SCAN_WINDOW_MS)
            if (_state.value.phase == Phase.SCANNING) {
                stopScanInternal()
                log.event("本轮扫描结束（${hits.size} 台匹配），未完成连接")
                if (!userDisconnected) {
                    // v3.0.1：不再把扫到的地址缓存成 device，下一轮照样重新扫描
                    scheduleReconnect()
                } else {
                    _state.update { it.copy(phase = Phase.IDLE) }
                }
            }
        }
    }

    /**
     * v3.0.1：ScanRecord.getServiceUuids() 只覆盖 16/32 位 UUID，128 位服务 UUID 不在其中，
     * 需要解析原始广播包（AD type 0x06/0x07，128 位 UUID 按小端存放）。
     * 仅作辅助匹配，主路径仍是设备名前缀——解析异常不影响原有行为。
     */
    private fun scanRecordHasService(rec: ScanRecord?): Boolean {
        val data = runCatching { rec?.bytes }.getOrNull() ?: return false
        var i = 0
        while (i + 1 < data.size) {
            val len = data[i].toInt() and 0xFF
            if (len == 0) break
            if (i + len >= data.size) break
            val type = data[i + 1].toInt() and 0xFF
            if (type == 0x06 || type == 0x07) {   // 128 位服务 UUID：完整 / 不完整列表
                var p = i + 2
                while (p + 16 <= i + 1 + len) {
                    val bb = ByteBuffer.wrap(data, p, 16).order(ByteOrder.LITTLE_ENDIAN)
                    val lsb = bb.long     // 前 8 字节 = 低 64 位
                    val msb = bb.long     // 后 8 字节 = 高 64 位
                    if (UUID(msb, lsb) == BleProto.SERVICE_UUID) return true
                    p += 16
                }
            }
            i += len + 1
        }
        return false
    }

    private fun stopScanInternal() {
        scanJob?.cancel(); scanJob = null
        val cb = scanCallback ?: return
        scanCallback = null
        try {
            adapter?.bluetoothLeScanner?.stopScan(cb)
        } catch (_: Exception) {
        }
    }

    // =====================================================================
    // 连接 / 重连
    // =====================================================================

    /**
     * v3.0.1：连接前先摘掉旧连接，并在协程里串行完成 disconnect → 等 close 回流 → connectGatt。
     * 绝不在 pending connect 上 close——厂商栈会因此把该设备标记为异常，后续 connectGatt 恒回 133。
     * 冷却期内 phase 仍是 CONNECTING，用户此时点连接会被 scanAndConnect 取消。
     */
    private fun connectDirect(dev: BluetoothDevice) {
        failPendingOps()
        stepTimeoutJob?.cancel(); stepTimeoutJob = null
        // 先同步摘掉旧 gatt，让旧回调立刻失效（避免"旧连接 DISCONNECTED → 又触发一轮重连"的竞态）
        val old = detachGatt()
        _state.update {
            it.copy(
                phase = Phase.CONNECTING,
                deviceName = runCatching { dev.name }.getOrNull() ?: BleProto.DEVICE_NAME_PREFIX,
                deviceAddress = dev.address,
            )
        }
        log.event("连接 ${_state.value.deviceName}（${dev.address}），第 ${retryAttempt.get() + 1} 次尝试")
        connectStartedAtMs = SystemClock.elapsedRealtime()

        scope.launch {
            if (old != null) {
                log.event("释放上一次 GATT 连接（${GATT_CLOSE_GRACE_MS}ms 冷却）")
                runCatching { old.disconnect() }
                delay(GATT_CLOSE_GRACE_MS)
                runCatching { old.close() }
                delay(100)   // close 回流
            }
            if (_state.value.phase != Phase.CONNECTING) return@launch
            val newGatt = try {
                dev.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            } catch (e: Exception) {
                log.error("connectGatt 异常: ${e.message}")
                null
            }
            if (newGatt == null) {
                scheduleReconnect()
                return@launch
            }
            gatt = newGatt
            armStepTimeout(CONNECT_TIMEOUT_MS, "连接超时(${CONNECT_TIMEOUT_MS / 1000}s)，本轮放弃")
        }
    }

    /**
     * @param overrideDelayMs 非空时覆盖退避阶梯（用于 133 恢复等特殊场景）
     */
    private fun scheduleReconnect(overrideDelayMs: Long? = null) {
        reconnectJob?.cancel()
        val attempt = retryAttempt.getAndIncrement()
        val delayMs = overrideDelayMs
            ?: RETRY_LADDER_MS[minOf(attempt, RETRY_LADDER_MS.size - 1)]
        val nextAt = System.currentTimeMillis() + delayMs
        _state.update { it.copy(phase = Phase.RECONNECTING, retryAttempt = attempt + 1, nextRetryAtMs = nextAt) }
        if (attempt == 0) {
            log.event("进入自动重连：${delayMs} ms 后第 1 次尝试")
        } else {
            log.event("重连退避 ${delayMs} ms（第 ${attempt + 1} 次）")
        }
        reconnectJob = scope.launch {
            delay(delayMs)
            attemptReconnect()
        }
    }

    private fun attemptReconnect() {
        if (userDisconnected) return
        val ad = adapter
        if (ad == null || !ad.isEnabled) {
            _state.update { it.copy(phase = Phase.BLUETOOTH_OFF) }
            log.error("蓝牙已关闭，等待蓝牙重新开启后自动重连")
            return
        }
        // v3.0.1：一律重新扫描。手表断连后要 10~40s 才恢复广播，对缓存地址盲连必然失败；
        // 而失败后放弃 pending connect 正是协议栈残留（后续恒 133）的来源。
        device = null
        beginScan()
    }

    /**
     * @param fromCallback true = 收到 onConnectionStateChange(DISCONNECTED)，链路已断，可立即 close；
     *                     false = 本地超时/异常发起，连接可能仍在 pending，必须先 disconnect 再延迟 close。
     */
    private fun handleDisconnected(status: Int, fromCallback: Boolean = false) {
        stepTimeoutJob?.cancel(); stepTimeoutJob = null
        periodicSyncJob?.cancel(); periodicSyncJob = null
        failPendingOps()
        releaseGatt(if (fromCallback) 0L else GATT_CLOSE_GRACE_MS)
        // v3.0.1：断开即清空缓存设备与特征，确保下一次一定走"扫描 → 发现 → 连接"
        device = null
        statusChar = null
        dataUploadChar = null
        val elapsed = if (connectStartedAtMs > 0) SystemClock.elapsedRealtime() - connectStartedAtMs else 0L

        // v3.0.1：连续 133 单独处理——不要原地重试，冷却 1s 后重新扫描
        val hit133 = status == 0x85 || status == 133
        if (hit133) {
            val n = gatt133Count.incrementAndGet()
            log.error("GATT_ERROR(133) 第 $n 次（本轮耗时 ${elapsed}ms）")
            if (n >= 2) {
                gatt133Count.set(0)
                retryAttempt.set(0)
                log.error("连续 133：协议栈可能残留 client，冷却 ${RECOVERY_COOLDOWN_MS}ms 后重新扫描")
                if (!userDisconnected) {
                    scheduleReconnect(RECOVERY_COOLDOWN_MS)
                }
                updateService()
                return
            }
        } else {
            gatt133Count.set(0)
        }

        if (userDisconnected) {
            _state.update { it.copy(phase = Phase.IDLE, retryAttempt = 0, nextRetryAtMs = 0L) }
            log.event("已断开")
        } else {
            log.error("连接断开：${BleProto.gattStatusMessage(status)}，本轮耗时 ${elapsed}ms，进入自动重连")
            _events.tryEmit("连接断开，自动重连中")
            scheduleReconnect()
        }
        updateService()
    }

    private fun armStepTimeout(ms: Long, what: String) {
        stepTimeoutJob?.cancel()
        stepTimeoutJob = scope.launch {
            delay(ms)
            log.error("$what")
            handleDisconnected(0x08)
        }
    }

    private fun failPendingOps() {
        // v3.0.1：丢弃断线前积压的写任务，避免重连成功后补发过期指令
        while (true) {
            val w = writeQueue.tryReceive().getOrNull() ?: break
            w.deferred.complete(-100)
        }
        writeResults.values.forEach { it.complete(-100) }
        writeResults.clear()
        descriptorResults.values.forEach { it.complete(-100) }
        descriptorResults.clear()
        readResults.values.forEach { it.complete(null) }
        readResults.clear()
        readySignal.value = false
    }

    /** 摘掉 gatt 引用：后续回调会因 `g !== gatt` 被忽略 */
    private fun detachGatt(): BluetoothGatt? {
        val g = gatt
        gatt = null
        return g
    }

    /**
     * v3.0.1：释放旧连接。
     * 先 detach 让旧回调失效，再 disconnect；graceMs 后 close（异步，不阻塞调用方）。
     * @param graceMs 0 = 链路已断可立即 close；>0 = 等状态机/协议栈回流后再 close
     */
    private fun releaseGatt(graceMs: Long) {
        val g = detachGatt() ?: return
        scope.launch {
            runCatching { g.disconnect() }
            if (graceMs > 0) delay(graceMs)
            runCatching { g.close() }
        }
    }

    // =====================================================================
    // GATT 回调
    // =====================================================================

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            val elapsed = if (connectStartedAtMs > 0) SystemClock.elapsedRealtime() - connectStartedAtMs else 0L
            scope.launch {
                if (g !== gatt) {
                    log.event("忽略过期 GATT 回调（newState=$newState status=0x%02X）".format(status))
                    return@launch
                }
                // v3.0.1：status 必须进日志——133 与 0 的差别直接决定重连策略
                log.event(
                    "连接状态变更 newState=$newState status=0x%02X（本轮 ${elapsed}ms，第 ${retryAttempt.get() + 1} 次）"
                        .format(status)
                )
                when (newState) {
                    BluetoothProfile.STATE_CONNECTED -> {
                        if (status != BluetoothGatt.GATT_SUCCESS) {
                            log.error("链路已建立但 status=0x%02X，按失败处理".format(status))
                            handleDisconnected(status, fromCallback = true)
                            return@launch
                        }
                        gatt133Count.set(0)
                        log.event("GATT 已连接，开始服务发现")
                        _state.update { it.copy(phase = Phase.INITIALIZING) }
                        armStepTimeout(STEP_TIMEOUT_MS, "服务发现超时")
                        try {
                            if (!g.discoverServices()) {
                                log.error("discoverServices 调用失败")
                                handleDisconnected(status)
                            }
                        } catch (e: Exception) {
                            log.error("discoverServices 异常: ${e.message}")
                            handleDisconnected(status)
                        }
                    }
                    BluetoothProfile.STATE_DISCONNECTED -> {
                        handleDisconnected(status, fromCallback = true)
                    }
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            scope.launch {
                if (g !== gatt) return@launch
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    log.error("服务发现失败: ${BleProto.gattStatusMessage(status)}")
                    handleDisconnected(status)
                    return@launch
                }
                val svc = g.getService(BleProto.SERVICE_UUID)
                statusChar = svc?.getCharacteristic(BleProto.STATUS_UUID)
                dataUploadChar = svc?.getCharacteristic(BleProto.DATA_UPLOAD_UUID)
                if (svc == null || statusChar == null || dataUploadChar == null) {
                    log.error("未找到 AI-Watch 服务或特征（服务/特征缺失）")
                    handleDisconnected(0x13)
                    return@launch
                }
                armStepTimeout(STEP_TIMEOUT_MS, "MTU 协商超时")
                log.event("服务已发现，请求 MTU $REQUEST_MTU")
                try {
                    if (!g.requestMtu(REQUEST_MTU)) {
                        log.error("requestMtu 调用失败")
                        handleDisconnected(0x13)
                    }
                } catch (e: Exception) {
                    log.error("requestMtu 异常: ${e.message}")
                    handleDisconnected(0x13)
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            scope.launch {
                if (g !== gatt) return@launch
                _state.update { it.copy(mtu = mtu) }
                log.event("MTU 协商完成: $mtu (status=0x%02X)".format(status))
                if (mtu - 3 < BleProto.COMMAND_HEADER_LEN + BleProto.MAX_TITLE_BYTES) {
                    log.error("警告：MTU 过小 (${mtu - 3} 字节有效载荷)，提醒帧可能不完整")
                }

                armStepTimeout(STEP_TIMEOUT_MS, "订阅 Status 超时")
                val ok1 = enableNotify(statusChar)
                if (!ok1) {
                    log.error("订阅 Status(f1) CCCD 失败")
                    handleDisconnected(0x13)
                    return@launch
                }
                log.event("已订阅 Status(f1)")

                armStepTimeout(STEP_TIMEOUT_MS, "订阅 DataUpload 超时")
                val ok3 = enableNotify(dataUploadChar)
                if (!ok3) {
                    log.error("订阅 DataUpload(f3) CCCD 失败")
                    handleDisconnected(0x13)
                    return@launch
                }
                log.event("已订阅 DataUpload(f3)")

                armStepTimeout(STEP_TIMEOUT_MS, "读取 Status 超时")
                val pair = readCharacteristic(statusChar)
                if (pair != null) {
                    val (bytes, st) = pair
                    if (st == 0) {
                        val s = BleProto.parseStatus(bytes)
                        if (s != null) {
                            _state.update { it.copy(lastSyncUtc = s.lastSyncUtc) }
                            log.rx("Status(f1)", bytes, "读：conn=${if (s.connected) 1 else 0} last_sync=${s.lastSyncUtc}")
                        } else {
                            log.rx("Status(f1)", bytes, "读：帧非法（长度/版本），已忽略")
                        }
                    } else {
                        log.error("读 Status(f1) 失败: ${BleProto.gattStatusMessage(st)}")
                    }
                }

                // §4 标准时序：MTU 完成后立即校时；先放行写队列
                readySignal.value = true
                enqueueTimeSync("连接建立自动校时")

                _state.update { it.copy(phase = Phase.CONNECTED, retryAttempt = 0, nextRetryAtMs = 0L) }
                stepTimeoutJob?.cancel(); stepTimeoutJob = null
                startPeriodicSync()
                _events.tryEmit("已连接 ${_state.value.deviceName ?: BleProto.DEVICE_NAME_PREFIX}")
                updateService()
                log.event("初始化完成，进入已连接状态")
            }
        }

        override fun onCharacteristicRead(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            // Android 12 及以下的旧签名
            @Suppress("DEPRECATION")
            val value = characteristic.value ?: ByteArray(0)
            scope.launch { handleReadResult(characteristic.uuid, value, status) }
        }

        override fun onCharacteristicRead(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray, status: Int) {
            // Android 13+ 新签名
            scope.launch { handleReadResult(characteristic.uuid, value, status) }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            scope.launch { writeResults[characteristic.uuid]?.complete(status) }
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            // Android 12 及以下：值挂在 characteristic 上
            @Suppress("DEPRECATION")
            val value = characteristic.value ?: return
            scope.launch { handleNotify(characteristic.uuid, value) }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
            // Android 13+
            scope.launch { handleNotify(characteristic.uuid, value) }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            scope.launch { descriptorResults[descriptor.characteristic?.uuid]?.complete(status) }
        }
    }

    private suspend fun handleReadResult(uuid: UUID, value: ByteArray, status: Int) {
        readResults[uuid]?.complete(value to status)
    }

    // =====================================================================
    // GATT 原语（挂起封装）
    // =====================================================================

    private suspend fun enableNotify(char: BluetoothGattCharacteristic?): Boolean {
        val g = gatt ?: return false
        if (char == null) return false
        return try {
            g.setCharacteristicNotification(char, true)
            val cccd = char.getDescriptor(BleProto.CCCD_UUID) ?: run {
                log.error("特征 ${char.uuid} 无 CCCD 描述符")
                return false
            }
            val payload = byteArrayOf(0x02, 0x00) // ENABLE_NOTIFICATION
            val d = CompletableDeferred<Int>()
            descriptorResults[char.uuid] = d
            val initiated = if (Build.VERSION.SDK_INT >= 33) {
                g.writeDescriptor(cccd, payload) == 0
            } else {
                @Suppress("DEPRECATION")
                cccd.value = payload
                @Suppress("DEPRECATION")
                g.writeDescriptor(cccd)
            }
            if (!initiated) {
                descriptorResults.remove(char.uuid)
                false
            } else {
                val st = withTimeoutOrNull(8_000) { d.await() } ?: -300
                descriptorResults.remove(char.uuid)
                st == 0
            }
        } catch (e: Exception) {
            descriptorResults.remove(char.uuid)
            log.error("订阅异常: ${e.message}")
            false
        }
    }

    private suspend fun readCharacteristic(char: BluetoothGattCharacteristic?): Pair<ByteArray, Int>? {
        val g = gatt ?: return null
        if (char == null) return null
        return try {
            val d = CompletableDeferred<Pair<ByteArray, Int>?>()
            readResults[char.uuid] = d
            val initiated: Boolean = try {
                // readCharacteristic 在所有 API 级别都返回 Boolean（true=已发起）
                g.readCharacteristic(char)
            } catch (e: Exception) {
                false
            }
            if (!initiated) {
                readResults.remove(char.uuid)
                null
            } else {
                withTimeoutOrNull(8_000) { d.await() }.also { readResults.remove(char.uuid) }
            }
        } catch (e: Exception) {
            readResults.remove(char.uuid)
            log.error("读特征异常: ${e.message}")
            null
        }
    }

    private suspend fun performWrite(uuid: UUID, bytes: ByteArray, tag: String, note: String): Int {
        val g = gatt ?: return -1
        val char = g.getService(BleProto.SERVICE_UUID)?.getCharacteristic(uuid) ?: run {
            log.error("特征不可用: $uuid")
            return -1
        }
        // MTU 门控：帧长不得超过有效载荷（AI_TEXT 最长 233 字节，必须先完成 MTU≥247 协商）
        val payloadLimit = _state.value.mtu - 3
        if (bytes.size > payloadLimit) {
            log.error("写入被拒($tag)：帧长 ${bytes.size} 超过 MTU 有效载荷 $payloadLimit（MTU=${_state.value.mtu}）")
            return -1100
        }
        return try {
            val d = CompletableDeferred<Int>()
            writeResults[uuid] = d
            val initiated: Boolean = if (Build.VERSION.SDK_INT >= 33) {
                // 新重载返回 Int 状态码（0 = BluetoothStatusCodes.SUCCESS）
                g.writeCharacteristic(char, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == 0
            } else {
                @Suppress("DEPRECATION")
                char.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                @Suppress("DEPRECATION")
                char.value = bytes
                @Suppress("DEPRECATION")
                g.writeCharacteristic(char)
            }
            if (!initiated) {
                writeResults.remove(uuid)
                log.error("写入发起失败($tag)")
                return -2
            }
            log.tx(tag, bytes, note)
            val st = withTimeoutOrNull(WRITE_TIMEOUT_MS) { d.await() } ?: -300
            writeResults.remove(uuid)
            if (st != 0) log.error("写入被拒($tag)：${BleProto.gattStatusMessage(st)}")
            st
        } catch (e: Exception) {
            writeResults.remove(uuid)
            log.error("写入异常($tag): ${e.message}")
            -3
        }
    }

    // =====================================================================
    // 串行写队列
    // =====================================================================

    private suspend fun enqueueWrite(uuid: UUID, bytes: ByteArray, tag: String, note: String): Int {
        val d = CompletableDeferred<Int>()
        pendingCount.incrementAndGet()
        refreshQueueSize()
        writeQueue.send(QueuedWrite(uuid, bytes, tag, note, d))
        val st = withTimeoutOrNull(ENQUEUE_TIMEOUT_MS) { d.await() } ?: -300
        pendingCount.decrementAndGet()
        refreshQueueSize()
        return st
    }

    private suspend fun processWrite(w: QueuedWrite) {
        while (true) {
            if (!readySignal.value) {
                w.deferred.complete(-100)
                return
            }
            val st = performWrite(w.uuid, w.bytes, w.tag, w.note)
            if (st == 0) {
                w.deferred.complete(0)
                return
            }
            // §3.4：0x11 队列满 → 稍后重试（2s 退避）
            if (st == 0x11 && w.retry < 5) {
                w.retry++
                log.error("手表队列已满(0x11)，2s 后重试 ${w.retry}/5：${w.note}")
                delay(2_000)
                continue
            }
            w.deferred.complete(st)
            return
        }
    }

    private fun refreshQueueSize() {
        _state.update { it.copy(queueSize = pendingCount.get()) }
    }

    private suspend fun enqueueTimeSync(reason: String): Boolean {
        val frame = BleProto.buildTimeSync() ?: run {
            log.error("TimeSync 帧构造失败（UTC/时区越界）")
            return false
        }
        val tz = BleProto.currentTzOffsetMinutes()
        val utc = System.currentTimeMillis() / 1000
        val st = enqueueWrite(
            BleProto.TIME_SYNC_UUID, frame, "TimeSync(f2)",
            "$reason：UTC=$utc 时区=${tz}分",
        )
        return if (st == 0) {
            _state.update { it.copy(lastSyncUtc = utc) }
            log.event("校时成功（$reason）")
            true
        } else {
            log.error("校时失败（$reason）：${BleProto.gattStatusMessage(st)}")
            false
        }
    }

    private fun startPeriodicSync() {
        periodicSyncJob?.cancel()
        periodicSyncJob = scope.launch {
            while (isActive && _state.value.phase == Phase.CONNECTED) {
                delay(PERIODIC_SYNC_MS)
                if (_state.value.phase == Phase.CONNECTED) {
                    enqueueTimeSync("8 小时周期兜底校时")
                }
            }
        }
    }

    // =====================================================================
    // 通知分发
    // =====================================================================

    private fun handleNotify(uuid: UUID, value: ByteArray) {
        when (uuid) {
            BleProto.STATUS_UUID -> {
                val s = BleProto.parseStatus(value)
                if (s == null) {
                    log.rx("Status(f1)", value, "通知：帧非法（长度/版本），已忽略")
                    return
                }
                _state.update { it.copy(lastSyncUtc = s.lastSyncUtc) }
                log.rx("Status(f1)", value, "通知：conn=${if (s.connected) 1 else 0} last_sync=${s.lastSyncUtc}")
            }
            BleProto.DATA_UPLOAD_UUID -> {
                if (value.size < 2) {
                    log.rx("DataUpload(f3)", value, "通知：帧过短，已忽略")
                    return
                }
                when (value[1].toInt() and 0xFF) {
                    // ---- v2：AI 触发（0x10） ----
                    BleProto.SENSOR_AI_TRIGGER -> {
                        val trigger = BleProto.parseAiTrigger(value)
                        if (trigger == null) {
                            log.rx("DataUpload(f3)", value, "通知：AI 触发帧非法（版本/长度），已忽略")
                            return
                        }
                        val srcText = when (trigger.source) {
                            BleProto.AI_SRC_ASSISTANT_PAGE -> "AI 助手页"
                            BleProto.AI_SRC_HR_PAGE -> "心率/血氧页(心率=${trigger.hr} 血氧=${trigger.spo2})"
                            else -> "未知来源 0x%02X".format(trigger.source)
                        }
                        log.rx("DataUpload(f3)", value, "通知：AI 触发 source=$srcText")
                        if (trigger.knownSource) {
                            aiTriggerListener?.invoke(trigger.source, trigger.hr, trigger.spo2)
                        } else {
                            log.error("未知 AI 触发来源，忽略（不崩溃）")
                        }
                    }

                    // ---- v3：今日步数（0x05） ----
                    BleProto.SENSOR_STEPS -> {
                        val s = BleProto.parseSteps(value)
                        if (s == null) {
                            log.rx("DataUpload(f3)", value, "通知：STEPS 帧非法，已忽略")
                            return
                        }
                        _state.update { it.copy(steps = s.steps) }
                        log.rx("DataUpload(f3)", value, "通知：今日步数 ${s.steps}")
                    }

                    // ---- v3：活动状态（0x06） ----
                    BleProto.SENSOR_ACTIVITY -> {
                        val a = BleProto.parseActivity(value)
                        if (a == null) {
                            log.rx("DataUpload(f3)", value, "通知：ACTIVITY 帧非法，已忽略")
                            return
                        }
                        _state.update { it.copy(activity = a.activity) }
                        log.rx("DataUpload(f3)", value, "通知：活动状态 ${BleProto.activityName(a.activity)}")
                    }

                    // ---- v3：疑似跌倒（0x11，不允许丢） ----
                    BleProto.SENSOR_FALL -> {
                        val f = BleProto.parseFallEvent(value)
                        if (f == null) {
                            log.rx("DataUpload(f3)", value, "通知：FALL 帧非法，已忽略")
                            return
                        }
                        _state.update { it.copy(fallEvent = f) }
                        log.rx("DataUpload(f3)", value, "通知：${f.label} impact=${f.impactMg}mg angle=${f.angleDeg}°")
                        fallListener?.invoke(f)
                    }

                    // ---- v3：原始运动流（0x12，高频，日志节流） ----
                    BleProto.SENSOR_MOTION -> {
                        val m = BleProto.parseMotionSample(value)
                        if (m == null) {
                            log.rx("DataUpload(f3)", value, "通知：MOTION 帧非法，已忽略")
                            return
                        }
                        _state.update { it.copy(motionCount = it.motionCount + 1) }
                        if (++motionLogTick % MOTION_LOG_EVERY == 0) {
                            log.rx("DataUpload(f3)", value, "通知：原始运动 t=${m.tMs}ms a=(${m.x},${m.y},${m.z})mg g=(${m.gx},${m.gy},${m.gz})mdps（已收 ${_state.value.motionCount} 帧）")
                        }
                        motionListener?.invoke(m)
                    }

                    // ---- v1：温度/湿度/心率/血氧 ----
                    else -> {
                        val sample = BleProto.parseDataUpload(value)
                        if (sample == null) {
                            log.rx("DataUpload(f3)", value, "通知：帧非法（版本/长度），已忽略")
                            return
                        }
                        if (!sample.known) {
                            // §3.3：未知 sensor_type 必须忽略，不崩溃（向后兼容）
                            log.rx("DataUpload(f3)", value, "通知：未知传感器类型 0x%02X，已忽略".format(sample.sensorType))
                            return
                        }
                        val now = System.currentTimeMillis()
                        _state.update { s ->
                            val v = SensorValue(sample.value, now)
                            s.copy(
                                sensors = when (sample.sensorType) {
                                    BleProto.SENSOR_TEMPERATURE -> s.sensors.copy(temperature = v)
                                    BleProto.SENSOR_HUMIDITY -> s.sensors.copy(humidity = v)
                                    BleProto.SENSOR_HEART_RATE -> s.sensors.copy(heartRate = v)
                                    BleProto.SENSOR_SPO2 -> s.sensors.copy(spo2 = v)
                                    else -> s.sensors
                                },
                            )
                        }
                        log.rx("DataUpload(f3)", value, "通知：${describeSample(sample)}")
                    }
                }
            }
            else -> log.rx("Notify", value, "未知特征通知 ${uuid}，已忽略")
        }
    }

    private fun describeSample(sample: BleProto.SensorSample): String = when (sample.sensorType) {
        BleProto.SENSOR_TEMPERATURE -> "温度 %.1f℃".format(sample.value)
        BleProto.SENSOR_HUMIDITY -> "湿度 %.1f%%RH".format(sample.value)
        BleProto.SENSOR_HEART_RATE -> "心率 %.0f 次/分".format(sample.value)
        BleProto.SENSOR_SPO2 -> "血氧 %.0f%%".format(sample.value)
        else -> "未知样本"
    }

    // =====================================================================
    // 前台服务联动
    // =====================================================================

    private fun updateService() {
        val phase = _state.value.phase
        // 不保活 / 用户已断开（IDLE）→ 停服务；否则保活中确保服务在运行
        if (!keepAlive || phase == Phase.IDLE) {
            try {
                context.stopService(Intent(context, WatchForegroundService::class.java))
            } catch (_: Exception) {
            }
            return
        }
        try {
            ContextCompat.startForegroundService(context, Intent(context, WatchForegroundService::class.java))
        } catch (_: Exception) {
            // 后台启动前台服务受限（Android 12+），忽略：服务通常已在前台开启
        }
    }

    fun shutdown() {
        scope.cancel()
    }

    private fun formatUtc(sec: Long): String {
        if (sec <= 0) return "即时"
        return java.text.SimpleDateFormat("MM-dd HH:mm:ss", java.util.Locale.getDefault()).format(java.util.Date(sec * 1000))
    }
}
