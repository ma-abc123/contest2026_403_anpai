package com.aiwatch.companion.ai

import android.content.Context
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

private val Context.aiDataStore by preferencesDataStore(name = "ai_settings")

/** Base URL 预置项 */
data class BaseUrlPreset(val key: String, val label: String, val url: String) {
    val isCustom: Boolean get() = key == KEY_CUSTOM

    companion object {
        const val KEY_DASHSCOPE = "dashscope"
        const val KEY_MIMO = "mimo"
        const val KEY_SILICONFLOW = "siliconflow"
        const val KEY_CUSTOM = "custom"

        val ALL = listOf(
            BaseUrlPreset(KEY_DASHSCOPE, "阿里云百炼（Qwen 系列）", "https://dashscope.aliyuncs.com/compatible-mode/v1"),
            BaseUrlPreset(KEY_MIMO, "小米 MiMo（Token Plan）", "https://token-plan-cn.xiaomimimo.com/v1"),
            BaseUrlPreset(KEY_SILICONFLOW, "硅基流动（开源模型）", "https://api.siliconflow.cn/v1"),
            BaseUrlPreset(KEY_CUSTOM, "自定义 OpenAI 兼容服务…", ""),
        )

        fun byKey(key: String): BaseUrlPreset = ALL.firstOrNull { it.key == key } ?: ALL.last()
    }
}

/** 一组模型服务配置（OpenAI 兼容） */
@Serializable
data class ServiceConfig(
    val baseUrlPresetKey: String = BaseUrlPreset.KEY_DASHSCOPE,
    val customBaseUrl: String = "",
    val apiKey: String = "",
    val model: String = "",
) {
    /** 解析出实际 Base URL（去掉结尾斜杠） */
    fun resolveBaseUrl(): String {
        val url = if (baseUrlPresetKey == BaseUrlPreset.KEY_CUSTOM) customBaseUrl.trim()
        else BaseUrlPreset.byKey(baseUrlPresetKey).url
        return url.trimEnd('/')
    }

    val configured: Boolean get() = resolveBaseUrl().isNotEmpty() && apiKey.isNotBlank() && model.isNotBlank()
}

/** ASR / TTS 模式 */
enum class ServiceMode { SYSTEM, MODEL }

/** 三组独立配置 + 测试状态 */
@Serializable
data class AiConfig(
    // LLM（必配）
    val llm: ServiceConfig = ServiceConfig(),
    val llmTestOk: Boolean? = null,        // null=未测试
    val llmTestNote: String = "",
    // ASR
    val asrMode: Int = ServiceMode.SYSTEM.ordinal,
    val asr: ServiceConfig = ServiceConfig(),
    val asrTestOk: Boolean? = null,
    val asrTestNote: String = "",
    // TTS
    val ttsMode: Int = ServiceMode.SYSTEM.ordinal,
    val tts: ServiceConfig = ServiceConfig(),
    val ttsTestOk: Boolean? = null,
    val ttsTestNote: String = "",
) {
    val asrModeEnum: ServiceMode get() = if (asrMode == ServiceMode.MODEL.ordinal) ServiceMode.MODEL else ServiceMode.SYSTEM
    val ttsModeEnum: ServiceMode get() = if (ttsMode == ServiceMode.MODEL.ordinal) ServiceMode.MODEL else ServiceMode.SYSTEM

    /** LLM 是否可用于会话：已配置且最近一次测试未失败（未测试允许尝试） */
    val llmReady: Boolean get() = llm.configured && llmTestOk != false
}

/**
 * AI 服务配置仓库（DataStore，整份 JSON 存储）。
 * 注意：API Key 仅落在本机应用私有目录，掩码显示，不写入日志。
 */
class AiConfigRepository(private val context: Context) {

    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }
    private val key = stringPreferencesKey("ai_config_json")

    private val _config = context.aiDataStore.data.map { prefs ->
        runCatching { json.decodeFromString<AiConfig>(prefs[key] ?: "") }.getOrDefault(AiConfig())
    }

    val config: Flow<AiConfig> = _config

    suspend fun current(): AiConfig = _config.first()

    suspend fun update(transform: (AiConfig) -> AiConfig) {
        val next = transform(current())
        context.aiDataStore.edit { prefs ->
            prefs[key] = json.encodeToString(AiConfig.serializer(), next)
        }
    }

    suspend fun setLlm(cfg: ServiceConfig) = update { it.copy(llm = cfg) }
    suspend fun setAsr(cfg: ServiceConfig) = update { it.copy(asr = cfg) }
    suspend fun setTts(cfg: ServiceConfig) = update { it.copy(tts = cfg) }
    suspend fun setAsrMode(mode: ServiceMode) = update { it.copy(asrMode = mode.ordinal) }
    suspend fun setTtsMode(mode: ServiceMode) = update { it.copy(ttsMode = mode.ordinal) }

    suspend fun setLlmTest(ok: Boolean, note: String) = update { it.copy(llmTestOk = ok, llmTestNote = note) }
    suspend fun setAsrTest(ok: Boolean, note: String) = update { it.copy(asrTestOk = ok, asrTestNote = note) }
    suspend fun setTtsTest(ok: Boolean, note: String) = update { it.copy(ttsTestOk = ok, ttsTestNote = note) }
}
