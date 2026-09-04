package com.aiwatch.companion.ai

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.aiwatch.companion.MainActivity
import com.aiwatch.companion.R
import com.aiwatch.companion.ble.BleProto
import com.aiwatch.companion.ble.Phase
import com.aiwatch.companion.ble.WatchBleManager
import com.aiwatch.companion.ble.WatchLog
import com.aiwatch.companion.data.ReminderRepository
import com.aiwatch.companion.ui.formatFull
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelChildren
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import java.io.File
import java.time.Instant
import java.util.concurrent.atomic.AtomicInteger

/** 会话阶段（§2.1 状态机） */
enum class AiStage { IDLE, LISTENING, RECOGNIZING, THINKING, DELIVERING, DONE, FAILED, CANCELED }

/** 单个动作的下发记录 */
@Serializable
data class AiActionRecord(
    val type: String,          // ai_text / create_reminder / start_timer / notify
    val title: String,
    val param: String = "",    // 触发时刻 / 时长等人类可读参数
    val result: String = "",
    val ok: Boolean = false,
)

/** 一条 AI 会话（历史持久化） */
@Serializable
data class AiSessionRecord(
    val id: Long,
    val timeMs: Long,
    val source: Int = BleProto.AI_SRC_MANUAL,
    val hr: Int = 0,
    val spo2: Int = 0,
    val userText: String? = null,
    val reply: String? = null,
    val actions: List<AiActionRecord> = emptyList(),
    val stage: String = AiStage.IDLE.name,
    val error: String? = null,
) {
    val busy: Boolean get() = stage in setOf(AiStage.LISTENING.name, AiStage.RECOGNIZING.name, AiStage.THINKING.name, AiStage.DELIVERING.name)
}

/** LLM 返回的动作（校验后） */
private data class ParsedAction(
    val type: String,
    val title: String,
    val triggerUtc: Long? = null,
    val durationS: Long? = null,
)

/** 后台触发的待启动请求（通知点击后由 UI 消费） */
data class TriggerInfo(val source: Int, val hr: Int, val spo2: Int)

/**
 * AI 会话状态机：
 * 手表触发(f3 0x10) → 聆听(录音/系统识别) → 识别(ASR) → 思考(LLM) → 下发(先 AI_TEXT 再动作) → 就绪
 * 超时：录音≤15s、ASR/LLM≤20s（AiHttp 内置）；断连/取消 → 终止不重放；0x11 队列满由写队列统一退避重试。
 */
class AiSessionManager(
    private val context: Context,
    private val ble: WatchBleManager,
    private val reminders: ReminderRepository,
    val configRepo: AiConfigRepository,
    private val log: WatchLog,
) {
    companion object {
        const val MAX_SESSIONS = 50
        const val CHANNEL_AI_TRIGGER = "ai_trigger"
        const val NOTIF_AI_TRIGGER = 2001
        const val EXTRA_AI_SOURCE = "ai_source"
        const val EXTRA_AI_HR = "ai_hr"
        const val EXTRA_AI_SPO2 = "ai_spo2"

        /** §2.2 system prompt（可直接使用），占位符运行期替换 */
        val SYSTEM_PROMPT = """
            你是智能手表的语音助手。用户的话会转成文本给你。只输出一个 JSON 对象,不要输出任何其他文字:
            {"reply": "<给用户的简短中文回复,不超过 70 个汉字>",
             "actions": [<0 个或多个动作>]}
            动作只能是以下三种之一:
            {"type":"create_reminder","title":"<提醒标题,不超过 8 个汉字>","trigger_utc":<触发时刻的 Unix 秒>}
            {"type":"start_timer","label":"<倒计时名称,不超过 8 个汉字>","duration_s":<1..86400 的整数>}
            {"type":"notify","title":"<通知标题,不超过 8 个汉字>"}
            规则:
            1. 用户要"N 分钟/小时后做某事"→ 用 create_reminder,不要用 start_timer。
            2. 用户要"N 分钟倒计时/计时"(如泡面、专注)→ 用 start_timer。
            3. 只是闲聊、提问、需要解释 → actions 为空数组。
            4. 回复要口语化、简短,适合手表屏幕。
            5. 计算 create_reminder 的 trigger_utc 时:以当前 Unix 秒 {utc_sec} 为基准,直接加上延迟秒数(1 分钟=60 秒,1 小时=3600 秒),得到的就是 trigger_utc。严禁做任何时区换算,严禁重新计算日期年份。
            当前 Unix 秒:{utc_sec},对应北京时间:{local_cn}。
        """.trimIndent()
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val json = Json { ignoreUnknownKeys = true }
    private val storeFile = File(context.filesDir, "ai_sessions.json")

    // ---------------- 状态 ----------------

    private val _active = MutableStateFlow<AiSessionRecord?>(null)
    val active: StateFlow<AiSessionRecord?> = _active.asStateFlow()

    private val _sessions = MutableStateFlow<List<AiSessionRecord>>(emptyList())
    val sessions: StateFlow<List<AiSessionRecord>> = _sessions.asStateFlow()

    /** 录音实时电平 0..1（UI 反馈） */
    private val _micLevel = MutableStateFlow(0f)
    val micLevel: StateFlow<Float> = _micLevel.asStateFlow()

    /** 通知点击带来的待启动触发（UI 消费后置 null） */
    val pendingAutoStart = MutableStateFlow<TriggerInfo?>(null)

    /** 应用是否在前台（MainActivity onResume/onPause 维护） */
    @Volatile
    var appInForeground = false

    // ---------------- id 计数器（1..255 独立循环） ----------------

    private val requestIdCounter = AtomicInteger(0)   // AI_TEXT
    private val timerIdCounter = AtomicInteger(0)     // AI_TIMER
    private var sessionSeq = 0L

    private var sessionJob: Job? = null
    private var recorder: AudioRecorder? = null
    private val systemAsr by lazy { SystemAsr(context) }

    init {
        loadSessions()
    }

    private fun loadSessions() {
        val loaded = runCatching {
            if (storeFile.exists()) json.decodeFromString<List<AiSessionRecord>>(storeFile.readText()) else null
        }.getOrNull()
        if (loaded != null) {
            _sessions.value = loaded
            sessionSeq = loaded.maxOfOrNull { it.id } ?: 0L
        }
    }

    private fun persistSessions(list: List<AiSessionRecord>) {
        runCatching {
            storeFile.writeText(json.encodeToString(kotlinx.serialization.builtins.ListSerializer(AiSessionRecord.serializer()), list))
        }
    }

    // =====================================================================
    // 触发入口
    // =====================================================================

    /** 手表 AI 触发 / 手动触发统一入口 */
    fun onTrigger(source: Int, hr: Int, spo2: Int) {
        if (ble.state.value.phase != Phase.CONNECTED) {
            log.error("AI 触发到达时手表未连接，忽略")
            return
        }
        if (_active.value?.busy == true) {
            log.error("已有 AI 会话进行中，忽略新触发")
            ble.notifyUser("已有 AI 会话进行中")
            return
        }
        scope.launch {
            val cfg = configRepo.current()
            if (!cfg.llmReady) {
                log.error("LLM 未配置或最近一次测试未通过，不进入聆听")
                ble.emitEvent("AI 服务未就绪，请先在设置页配置并测试 LLM")
                return@launch
            }
            if (appInForeground) {
                startSession(source, hr, spo2)
            } else {
                postTriggerNotification(source, hr, spo2)
            }
        }
    }

    /** §2.4：后台触发 → 高优先级通知，点开即录 */
    private fun postTriggerNotification(source: Int, hr: Int, spo2: Int) {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(context, android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            log.error("缺少通知权限，无法提示后台 AI 触发")
            return
        }
        val nm = context.getSystemService(android.app.NotificationManager::class.java)
        if (nm?.getNotificationChannel(CHANNEL_AI_TRIGGER) == null) {
            val ch = android.app.NotificationChannel(
                CHANNEL_AI_TRIGGER, "手表 AI 触发",
                android.app.NotificationManager.IMPORTANCE_HIGH,
            )
            nm?.createNotificationChannel(ch)
        }
        val intent = Intent(context, MainActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            putExtra(EXTRA_AI_SOURCE, source)
            putExtra(EXTRA_AI_HR, hr)
            putExtra(EXTRA_AI_SPO2, spo2)
        }
        val pi = PendingIntent.getActivity(
            context, 2001, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val notification = NotificationCompat.Builder(context, CHANNEL_AI_TRIGGER)
            .setSmallIcon(R.drawable.ic_stat_watch)
            .setContentTitle("手表 AI 触发")
            .setContentText("点按开始语音对话")
            .setPriority(NotificationCompat.PRIORITY_MAX)
            .setContentIntent(pi)
            .setAutoCancel(true)
            .setTimeoutAfter(30_000)
            .build()
        NotificationManagerCompat.from(context).notify(NOTIF_AI_TRIGGER, notification)
        log.event("应用在后台，已发送 AI 触发通知")
    }

    // =====================================================================
    // 会话流程
    // =====================================================================

    /** 启动一次完整会话（录音 → ASR → LLM → 下发） */
    fun startSession(source: Int, hr: Int, spo2: Int) {
        if (_active.value?.busy == true) {
            ble.notifyUser("已有 AI 会话进行中")
            return
        }
        if (ble.state.value.phase != Phase.CONNECTED) {
            ble.notifyUser("手表未连接")
            log.error("启动 AI 会话失败：手表未连接")
            return
        }
        sessionJob = scope.launch {
            runSession(source, hr, spo2, reuseText = null)
        }
    }

    /** 失败项重试：有识别文本 → 从 LLM 重跑；否则重新录音 */
    fun retry(sessionId: Long) {
        val rec = _sessions.value.firstOrNull { it.id == sessionId } ?: return
        if (!rec.userText.isNullOrBlank()) {
            if (_active.value?.busy == true) {
                ble.notifyUser("已有 AI 会话进行中")
                return
            }
            sessionJob = scope.launch {
                runSession(rec.source, rec.hr, rec.spo2, reuseText = rec.userText)
            }
        } else {
            startSession(rec.source, rec.hr, rec.spo2)
        }
    }

    /** 取消当前会话（录音阶段重点支持） */
    fun cancel() {
        recorder?.cancel()
        sessionJob?.cancel()
        sessionJob = null
        log.event("AI 会话已取消")
    }

    /** 删除单条会话记录 */
    fun deleteSession(id: Long) {
        _sessions.value = _sessions.value.filterNot { it.id == id }
        persistSessions(_sessions.value)
        log.event("已删除会话记录")
    }

    /** 清空全部会话记录 */
    fun clearSessions() {
        _sessions.value = emptyList()
        persistSessions(emptyList())
        log.event("已清空全部会话记录")
    }

    private suspend fun runSession(source: Int, hr: Int, spo2: Int, reuseText: String?) {
        _active.value = AiSessionRecord(
            id = ++sessionSeq,
            timeMs = System.currentTimeMillis(),
            source = source,
            hr = hr,
            spo2 = spo2,
            stage = AiStage.LISTENING.name,
        )
        recorder = null
        try {
            val cfg = configRepo.current()
            if (!cfg.llmReady) throw IllegalStateException("AI 服务未就绪，请检查设置")

            // ---------- LISTENING / RECOGNIZING ----------
            val userText = if (reuseText != null) {
                updateSession { it.copy(userText = reuseText, stage = AiStage.THINKING.name) }
                reuseText
            } else {
                SpeechPlayer.stopAll()
                val text = when (cfg.asrModeEnum) {
                    ServiceMode.SYSTEM -> {
                        // 系统识别器自带录音与 VAD
                        systemAsr.recognize()
                    }
                    ServiceMode.MODEL -> {
                        val rec = AudioRecorder(onAmplitude = { _micLevel.value = it })
                        recorder = rec
                        val wav = try {
                            rec.record() // LISTENING
                        } finally {
                            recorder = null
                            _micLevel.value = 0f
                        }
                        updateSession { it.copy(stage = AiStage.RECOGNIZING.name) }
                        AiHttp.transcribe(cfg.asr, wav) // RECOGNIZING
                    }
                }
                if (text.isBlank()) throw IllegalStateException("未识别到语音")
                updateSession { it.copy(userText = text, stage = AiStage.THINKING.name) }
                text
            }

            // ---------- THINKING ----------
            val (reply, actions) = callLlm(cfg, userText, source, hr, spo2)
            // 空回复保护：不下发空 AI_TEXT 到手表，直接判失败可重试
            if (reply.isBlank()) throw IllegalStateException("AI 返回了空回复（服务端偶发），请重试")
            updateSession { it.copy(reply = reply, stage = AiStage.DELIVERING.name) }

            // ---------- DELIVERING：先 AI_TEXT，再动作 ----------
            val delivered = mutableListOf<AiActionRecord>()
            delivered += deliverAiText(reply)
            for (a in actions) {
                delivered += deliverAction(a)
            }
            updateSession { it.copy(actions = delivered, stage = AiStage.DONE.name) }
            log.event("AI 会话完成：「${userText.take(30)}」→「${reply.take(40)}」 动作=${delivered.size - 1}")

            // ---------- TTS（失败不影响下发） ----------
            speak(cfg, reply)
        } catch (e: CancellationException) {
            finishSession(AiStage.CANCELED, "已取消")
            throw e
        } catch (e: Exception) {
            val msg = AiHttp.readableError(e)
            log.error("AI 会话失败：$msg")
            ble.emitEvent("AI 会话失败：$msg")
            finishSession(AiStage.FAILED, msg)
            return
        }
        finishSession(AiStage.DONE, null)
    }

    private suspend fun updateSession(mutate: (AiSessionRecord) -> AiSessionRecord) {
        val current = _active.value ?: return
        _active.value = mutate(current)
    }

    /** 会话终结：写入历史并清空活动位 */
    private fun finishSession(stage: AiStage, error: String?) {
        val s = _active.value ?: return
        val final = s.copy(stage = stage.name, error = error)
        _sessions.value = (listOf(final) + _sessions.value).take(MAX_SESSIONS)
        persistSessions(_sessions.value)
        _active.value = null
        _micLevel.value = 0f
    }

    // =====================================================================
    // LLM
    // =====================================================================

    private suspend fun callLlm(
        cfg: com.aiwatch.companion.ai.AiConfig,
        userText: String,
        source: Int,
        hr: Int,
        spo2: Int,
    ): Pair<String, List<ParsedAction>> {
        val utcSec = System.currentTimeMillis() / 1000
        val localCn = java.time.LocalDateTime.ofInstant(Instant.ofEpochSecond(utcSec), java.time.ZoneId.systemDefault())
            .format(java.time.format.DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss"))
        val sys = SYSTEM_PROMPT
            .replace("{utc_sec}", utcSec.toString())
            .replace("{local_cn}", localCn)
        val user = if (source == BleProto.AI_SRC_HR_PAGE) {
            val hrText = if (hr > 0) "$hr 次/分" else "未知"
            val spo2Text = if (spo2 > 0) "$spo2%" else "未知"
            "$userText\n（用户当前心率：$hrText；血氧：$spo2Text。若用户想分析这些数据，请基于它们回答）"
        } else {
            userText
        }
        val raw = AiHttp.chat(cfg.llm, sys, user)
        log.event("LLM 原始返回：${raw.take(200)}")
        return parseLlmJson(raw, userText)
    }

    /** 强制 JSON 解析；失败时降级：原文仅作为 AI_TEXT，不带动作 */
    private fun parseLlmJson(raw: String, userText: String): Pair<String, List<ParsedAction>> {
        val cleaned = raw.trim()
            .removePrefix("```json").removePrefix("```JSON").removePrefix("```")
            .removeSuffix("```").trim()
        val start = cleaned.indexOf('{')
        val end = cleaned.lastIndexOf('}')
        if (start < 0 || end <= start) return cleaned to emptyList()
        return try {
            val obj = json.parseToJsonElement(cleaned.substring(start, end + 1)).jsonObject
            val reply = obj["reply"]?.jsonPrimitive?.content.orEmpty()
            val actions = obj["actions"]?.jsonArray?.mapNotNull { el ->
                val a = el.jsonObject
                when (a["type"]?.jsonPrimitive?.content) {
                    "create_reminder" -> ParsedAction(
                        "create_reminder",
                        a["title"]?.jsonPrimitive?.content.orEmpty(),
                        triggerUtc = sanitizeTrigger(a["trigger_utc"]?.jsonPrimitive?.longOrNull, userText),
                    )
                    "start_timer" -> ParsedAction(
                        "start_timer",
                        a["label"]?.jsonPrimitive?.content.orEmpty(),
                        durationS = a["duration_s"]?.jsonPrimitive?.longOrNull,
                    )
                    "notify" -> ParsedAction("notify", a["title"]?.jsonPrimitive?.content.orEmpty())
                    else -> null // 未知动作忽略
                }
            }.orEmpty()
            if (reply.isBlank()) cleaned to actions else reply to actions
        } catch (e: Exception) {
            log.error("LLM JSON 解析失败，降级为纯文本回复：${e.message}")
            cleaned to emptyList()
        }
    }

    /**
     * 触发时间校验（App 端接管时间计算，不信任 LLM 的算术）：
     * 模型算出的 trigger_utc 若落在过去（时区/年份误算的典型症状），
     * 按用户原话提取的延迟秒数重算：now + 延迟；提取不到则默认 5 分钟。
     */
    private fun sanitizeTrigger(rawUtc: Long?, userText: String): Long {
        val now = System.currentTimeMillis() / 1000
        if (rawUtc != null && rawUtc > now + 60) {
            return rawUtc // 未来时间，合理，信任
        }
        val delay = extractDelaySeconds(userText) ?: 300L
        val fixed = now + delay
        log.event(
            "触发时间异常修正：模型给了 ${rawUtc ?: "null"}（已按「${userText.take(20)}」修正为 +${delay} 秒）",
        )
        return fixed
    }

    /** 从用户原话提取延迟秒数（支持阿拉伯数字与中文数字，如"5分钟""两小时后""30秒"） */
    private fun extractDelaySeconds(text: String): Long? {
        val m = Regex("([0-9零一二两三四五六七八九十百]+)\\s*(分钟|小时|秒钟?|秒)").find(text) ?: return null
        val n = parseCnNumber(m.groupValues[1]) ?: return null
        return when {
            m.groupValues[2] == "小时" -> n * 3600L
            m.groupValues[2] == "分钟" -> n * 60L
            else -> n * 1L
        }
    }

    /** 中文数字转整数（支持 零~九/十/百/两 及组合，如 十五=15、二十五=25、两=2） */
    private fun parseCnNumber(s: String): Int? {
        if (s.isEmpty()) return null
        if (s.all { it.isDigit() }) return s.toInt()
        val map = mapOf('零' to 0, '一' to 1, '二' to 2, '两' to 2, '三' to 3, '四' to 4, '五' to 5, '六' to 6, '七' to 7, '八' to 8, '九' to 9)
        var total = 0
        var current = 0
        for (ch in s) {
            when (ch) {
                '十' -> { if (current == 0) current = 1; total += current * 10; current = 0 }
                '百' -> { if (current == 0) current = 1; total += current * 100; current = 0 }
                else -> {
                    val d = map[ch] ?: return null
                    current = d
                }
            }
        }
        return total + current
    }

    // =====================================================================
    // 下发
    // =====================================================================

    private fun nextRequestId(): Int = nextCyclic(requestIdCounter)
    private fun nextTimerId(): Int = nextCyclic(timerIdCounter)

    private fun nextCyclic(counter: AtomicInteger): Int =
        counter.updateAndGet { cur -> if (cur >= 255) 1 else cur + 1 }

    private suspend fun deliverAiText(reply: String): AiActionRecord {
        val id = nextRequestId()
        val now = System.currentTimeMillis() / 1000
        val st = ble.sendCommand(
            BleProto.CMD_AI_TEXT, id, 0, now, reply,
            maxTitleBytes = BleProto.AI_TEXT_MAX_BYTES,
        )
        val ok = st == 0
        val result = if (ok) "成功" else failText(st)
        return AiActionRecord(
            type = "ai_text",
            title = BleProto.truncateTitle(reply, 60),
            param = "id=$id · ${formatFull(now * 1000)}",
            result = result,
            ok = ok,
        ).also {
            if (!ok) throw IllegalStateException("AI_TEXT 下发失败：$result（动作已停止，不部分下发）")
        }
    }

    private suspend fun deliverAction(a: ParsedAction): AiActionRecord = when (a.type) {
        "create_reminder" -> {
            val id = reminders.allocateId()
            val trigger = a.triggerUtc ?: (System.currentTimeMillis() / 1000 + 300)
            val st = ble.sendCommand(BleProto.CMD_REMINDER, id, BleProto.FLAG_ACTIVE, trigger, a.title)
            val ok = st == 0
            if (ok) {
                reminders.upsert(
                    ReminderRepository.ReminderEntity(
                        id = id, title = BleProto.truncateTitle(a.title),
                        cmdType = BleProto.CMD_REMINDER, flags = BleProto.FLAG_ACTIVE,
                        triggerEpochSec = trigger,
                        createdAt = System.currentTimeMillis(), updatedAt = System.currentTimeMillis(),
                        lastSendState = "成功", lastSendAt = System.currentTimeMillis(),
                    ),
                )
            }
            AiActionRecord("create_reminder", a.title, param = "触发 ${formatFull(trigger * 1000)}", result = if (ok) "成功" else failText(st), ok = ok)
        }
        "start_timer" -> {
            val id = nextTimerId()
            val dur = (a.durationS ?: 60L).coerceIn(BleProto.AI_TIMER_MIN_S, BleProto.AI_TIMER_MAX_S)
            val st = ble.sendCommand(BleProto.CMD_AI_TIMER, id, 0, dur, a.title)
            val ok = st == 0
            AiActionRecord("start_timer", a.title, param = "时长 ${dur / 60}分${dur % 60}秒", result = if (ok) "成功" else failText(st), ok = ok)
        }
        "notify" -> {
            val id = reminders.allocateId()
            val now = System.currentTimeMillis() / 1000
            val st = ble.sendCommand(BleProto.CMD_NOTIFY, id, 0, now, a.title)
            val ok = st == 0
            if (ok) {
                reminders.upsert(
                    ReminderRepository.ReminderEntity(
                        id = id, title = BleProto.truncateTitle(a.title),
                        cmdType = BleProto.CMD_NOTIFY, flags = 0,
                        triggerEpochSec = null,
                        createdAt = System.currentTimeMillis(), updatedAt = System.currentTimeMillis(),
                        lastSendState = "成功", lastSendAt = System.currentTimeMillis(),
                    ),
                )
            }
            AiActionRecord("notify", a.title, param = "即时", result = if (ok) "成功" else failText(st), ok = ok)
        }
        else -> AiActionRecord(a.type, a.title, result = "未知动作已忽略", ok = true)
    }

    private fun failText(st: Int): String = when (st) {
        -1100 -> "MTU 不足（未完成 ≥247 协商）"
        else -> BleProto.gattStatusMessage(st)
    }

    // =====================================================================
    // TTS
    // =====================================================================

    private suspend fun speak(cfg: com.aiwatch.companion.ai.AiConfig, text: String) {
        try {
            when (cfg.ttsModeEnum) {
                ServiceMode.SYSTEM -> {
                    SpeechPlayer.ensureTts(context)
                    SpeechPlayer.speak(text)
                }
                ServiceMode.MODEL -> {
                    val audio = AiHttp.synthesize(cfg.tts, text)
                    SpeechPlayer.playAudio(context, audio)
                }
            }
        } catch (e: Exception) {
            log.error("TTS 播报失败（不影响手表收文本）：${AiHttp.readableError(e)}")
        }
    }

    fun shutdown() {
        scope.coroutineContext.cancelChildren()
    }
}
