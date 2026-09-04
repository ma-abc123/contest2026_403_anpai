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
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
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
 * 扫描(名称前缀/服务UUID过滤) → 连接(15s超时) → MTU≥247 → 订阅 f1/f3 → 读 Status → TimeSync → 就绪
 * 断线自动重连（指数退避 2s→30s 封顶，持续重试）；用户主动断开不重连；
 * 蓝牙开关重开 / 应用回前台立即重连；时区变化补发校时；8 小时周期兜底校时。
 * 所有 GATT 写操作经串行队列，0x11 队列满按 2s 退避重试。
 */
@SuppressLint("MissingPermission")
class WatchBleManager(
    private val context: Application,
    val log: WatchLog,
) {
    companion object {
        const val CONNECT_TIMEOUT_MS = 15_000L      // §2 GATT 层连接超时建议 15s
        const val STEP_TIMEOUT_MS = 12_000L         // 初始化单步超时
        const val BASE_RETRY_MS = 2_000L            // §2 指数退避 2s 起
        const val MAX_RETRY_MS = 30_000L            // 封顶 30s
        const val PERIODIC_SYNC_MS = 8L * 3600_000L // §3.2 建议 6~12h 兜底，取 8h
        const val SCAN_WINDOW_MS = 20_000L          // 单轮扫描窗口
        const val WRITE_TIMEOUT_MS = 10_000L        // 单次写等待
        const val ENQUEUE_TIMEOUT_MS = 90_000L      // 入队结果等待（容忍重连）
        const val REQUEST_MTU = 247                 // §2 要求 ≥247
        const val MOTION_LOG_EVERY = 25             // v3：0x12 高频帧日志节流（8Hz 下约 3s 一条）
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
                                closeGattQuietly()
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

    /** 用户点击「扫描并连接」/ 应用启动恢复 */
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
        userDisconnected = false
        retryAttempt.set(0)
        reconnectJob?.cancel()
        reconnectJob = null
        val dev = device
        if (dev != null) connectDirect(dev) else beginScan()
        updateService()
    }

    /** 用户主动断开：不自动重连 */
    fun disconnectByUser() {
        userDisconnected = true
        reconnectJob?.cancel(); reconnectJob = null
        scanJob?.cancel(); scanJob = null
        stopScanInternal()
        failPendingOps()
        closeGattQuietly()
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

    /** 应用回到前台：立即触发一轮重连 */
    fun onAppForeground() {
        if (userDisconnected) return
        val p = _state.value.phase
        if (p == Phase.RECONNECTING || p == Phase.BLUETOOTH_OFF) {
            val ad = adapter
            if (ad != null && ad.isEnabled) {
                log.event("应用回前台，立即重连")
                reconnectJob?.cancel()
                retryAttempt.set(0)
                attemptReconnect()
            }
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
        _state.update { it.copy(phase = Phase.SCANNING, scanHits = emptyList()) }
        log.event("开始扫描（${BleProto.DEVICE_NAME_PREFIX} 前缀 / 服务 UUID 过滤）")

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
                val name = result.device?.name
                val serviceUuids = result.scanRecord?.serviceUuids
                val uuidMatch = serviceUuids?.any { it.uuid == BleProto.SERVICE_UUID } == true
                val nameMatch = name?.startsWith(BleProto.DEVICE_NAME_PREFIX) == true
                if (!uuidMatch && !nameMatch) return
                val addr = result.device?.address ?: return
                val firstHit = !hits.containsKey(addr)
                hits[addr] = ScanHit(name, addr, result.rssi)
                _state.update { s -> s.copy(scanHits = hits.values.toList()) }
                if (firstHit) {
                    log.event("发现设备 ${name ?: addr} (RSSI ${result.rssi})")
                }
                // 命中即连（首台自动连接；其余展示在列表中供手动选择）
                if (firstHit && _state.value.phase == Phase.SCANNING) {
                    val dev = result.device ?: return
                    device = dev
                    stopScanInternal()
                    scope.launch { connectDirect(dev) }
                }
            }

            override fun onScanFailed(errorCode: Int) {
                log.error("扫描失败 code=$errorCode")
                scope.launch {
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
                log.event("本轮扫描结束（${hits.size} 台匹配）")
                if (!userDisconnected) {
                    if (device == null && hits.isNotEmpty()) {
                        device = adapter?.getRemoteDevice(hits.values.first().address)
                    }
                    scheduleReconnect()
                } else {
                    _state.update { it.copy(phase = Phase.IDLE) }
                }
            }
        }
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

    private fun connectDirect(dev: BluetoothDevice) {
        failPendingOps()
        closeGattQuietly()
        _state.update {
            it.copy(
                phase = Phase.CONNECTING,
                deviceName = dev.name ?: BleProto.DEVICE_NAME_PREFIX,
                deviceAddress = dev.address,
            )
        }
        log.event("连接 ${dev.name ?: dev.address} …")
        val newGatt = try {
            dev.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } catch (e: Exception) {
            log.error("connectGatt 异常: ${e.message}")
            null
        }
        if (newGatt == null) {
            scheduleReconnect()
            return
        }
        gatt = newGatt
        armStepTimeout(CONNECT_TIMEOUT_MS, "连接超时(15s)")
    }

    private fun scheduleReconnect() {
        reconnectJob?.cancel()
        val attempt = retryAttempt.getAndIncrement()
        val delayMs = minOf(BASE_RETRY_MS shl minOf(attempt, 4), MAX_RETRY_MS)
        val nextAt = System.currentTimeMillis() + delayMs
        _state.update { it.copy(phase = Phase.RECONNECTING, retryAttempt = attempt + 1, nextRetryAtMs = nextAt) }
        if (attempt == 0) {
            log.event("进入自动重连：${delayMs / 1000}s 后第 1 次尝试")
        } else {
            log.event("重连退避 ${delayMs / 1000}s（第 ${attempt + 1} 次）")
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
        val dev = device
        when {
            dev == null -> beginScan()
            retryAttempt.get() >= 3 -> beginScan() // 连续直连失败，改走扫描定位
            else -> connectDirect(dev)
        }
    }

    private fun handleDisconnected(status: Int) {
        stepTimeoutJob?.cancel(); stepTimeoutJob = null
        periodicSyncJob?.cancel(); periodicSyncJob = null
        failPendingOps()
        closeGattQuietly()
        if (userDisconnected) {
            _state.update { it.copy(phase = Phase.IDLE, retryAttempt = 0, nextRetryAtMs = 0L) }
            log.event("已断开")
        } else {
            log.error("连接断开：${BleProto.gattStatusMessage(status)}，进入自动重连")
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
        writeResults.values.forEach { it.complete(-100) }
        writeResults.clear()
        descriptorResults.values.forEach { it.complete(-100) }
        descriptorResults.clear()
        readResults.values.forEach { it.complete(null) }
        readResults.clear()
        readySignal.value = false
    }

    private fun closeGattQuietly() {
        val g = gatt ?: return
        gatt = null
        try {
            g.disconnect()
        } catch (_: Exception) {
        }
        try {
            g.close()
        } catch (_: Exception) {
        }
    }

    // =====================================================================
    // GATT 回调
    // =====================================================================

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            scope.launch {
                if (g !== gatt) return@launch
                when (newState) {
                    BluetoothProfile.STATE_CONNECTED -> {
                        log.event("GATT 已连接 (status=0x%02X)".format(status))
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
                        handleDisconnected(status)
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
