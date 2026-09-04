package com.aiwatch.companion.ai

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import kotlinx.serialization.json.buildJsonArray
import okhttp3.Call
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.MultipartBody
import java.io.IOException
import java.util.concurrent.TimeUnit

/** AI 服务调用异常（消息面向用户，绝不包含 API Key） */
class AiHttpException(val code: Int, message: String) : Exception(message)

/**
 * OpenAI 兼容协议客户端（Qwen 百炼 / 小米 MiMo / 硅基流动 / 自定义网关）。
 * 约定：日志只记录 base host、模型名与状态码，绝不记录 Authorization。
 */
object AiHttp {

    private val json = Json { ignoreUnknownKeys = true }

    private val client: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(20, TimeUnit.SECONDS)   // §2.1 ASR/LLM ≤ 20s
        .writeTimeout(20, TimeUnit.SECONDS)
        .build()

    private val JSON_MEDIA = "application/json; charset=utf-8".toMediaType()

    // ------------------------------------------------------------------
    // Chat Completions（LLM）
    // ------------------------------------------------------------------

    /**
     * 发送一次对话。默认尝试 response_format=json_object，
     * 服务不支持时（400 且错误信息提到 response_format）自动去掉重试一次。
     * @return 助手回复文本（未解析 JSON 围栏，由调用方处理）
     */
    suspend fun chat(cfg: ServiceConfig, system: String, user: String): String = withContext(Dispatchers.IO) {
        ensureConfigured(cfg)
        val base = cfg.resolveBaseUrl()
        try {
            runChat(base, cfg.apiKey, cfg.model, system, user, jsonMode = true)
        } catch (e: AiHttpException) {
            if (e.code == 400 && e.message?.contains("response_format", ignoreCase = true) == true) {
                runChat(base, cfg.apiKey, cfg.model, system, user, jsonMode = false)
            } else {
                throw e
            }
        }
    }

    private fun runChat(base: String, key: String, model: String, system: String, user: String, jsonMode: Boolean): String {
        val mimo = isMimo(base)
        val body = buildJsonObject {
            put("model", model)
            put("temperature", 0.4)
            // 小米 MiMo 用 max_completion_tokens；其余服务用 max_tokens
            if (mimo) put("max_completion_tokens", 800) else put("max_tokens", 800)
            put("messages", buildJsonArray {
                add(buildJsonObject { put("role", "system"); put("content", system) })
                add(buildJsonObject { put("role", "user"); put("content", user) })
            })
            if (jsonMode) put("response_format", buildJsonObject { put("type", "json_object") })
            // 小米 MiMo 默认开启思维链，推理 token 会挤占输出额度导致 content 偶发为空 → 显式关闭
            if (mimo) put("thinking", buildJsonObject { put("type", "disabled") })
        }
        val request = Request.Builder()
            .url("$base/chat/completions")
            .header("Authorization", "Bearer $key")
            .post(body.toString().toRequestBody(JSON_MEDIA))
            .build()
        client.newCall(request).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) {
                val snippet = text.take(300).replace("\n", " ")
                throw AiHttpException(resp.code, "HTTP ${resp.code}：$snippet")
            }
            // content 优先；偶发空时回退 reasoning_content（部分模型把答案放思考字段）
            val msg = runCatching {
                json.parseToJsonElement(text)
                    .jsonObject["choices"]?.jsonArray?.firstOrNull()
                    ?.jsonObject?.get("message")?.jsonObject
            }.getOrNull()
            val content = msg?.get("content")?.jsonPrimitive?.content?.takeIf { it.isNotBlank() }
                ?: msg?.get("reasoning_content")?.jsonPrimitive?.content?.takeIf { it.isNotBlank() }
            return content ?: throw AiHttpException(resp.code, "模型未输出内容（响应缺少 message.content），请重试")
        }
    }

    // ------------------------------------------------------------------
    // Models
    // ------------------------------------------------------------------

    /** 拉取模型列表：qwen / mimo 置顶分组，其余按字母序 */
    suspend fun listModels(cfg: ServiceConfig): List<String> = withContext(Dispatchers.IO) {
        ensureConfigured(cfg, requireModel = false)
        val base = cfg.resolveBaseUrl()
        val request = Request.Builder()
            .url("$base/models")
            .header("Authorization", "Bearer ${cfg.apiKey}")
            .get()
            .build()
        client.newCall(request).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) {
                val snippet = text.take(200).replace("\n", " ")
                throw AiHttpException(resp.code, "HTTP ${resp.code}：$snippet")
            }
            val ids = runCatching {
                json.parseToJsonElement(text).jsonObject["data"]?.jsonArray
                    ?.mapNotNull { it.jsonObject["id"]?.jsonPrimitive?.content }
            }.getOrNull().orEmpty()
            val qwen = ids.filter { it.contains("qwen", ignoreCase = true) }.sorted()
            val mimo = ids.filter { it.contains("mimo", ignoreCase = true) }.sorted()
            val rest = (ids - qwen.toSet() - mimo.toSet()).sorted()
            qwen + mimo + rest
        }
    }

    // ------------------------------------------------------------------
    // Audio Transcriptions（ASR）
    // ------------------------------------------------------------------

    /** 整段 WAV 上传识别，返回文本。小米 MiMo 走 chat+input_audio，其余走标准 /audio/transcriptions */
    suspend fun transcribe(cfg: ServiceConfig, wav: ByteArray): String = withContext(Dispatchers.IO) {
        ensureConfigured(cfg)
        val base = cfg.resolveBaseUrl()
        if (isMimo(base)) {
            transcribeMimo(base, cfg.apiKey, cfg.model, wav)
        } else {
            val filePart = MultipartBody.Part.createFormData(
                "file", "audio.wav",
                wav.toRequestBody("audio/wav".toMediaType()),
            )
            val multipart = MultipartBody.Builder()
                .setType(MultipartBody.FORM)
                .addPart(filePart)
                .addFormDataPart("model", cfg.model)
                .build()
            val request = Request.Builder()
                .url("$base/audio/transcriptions")
                .header("Authorization", "Bearer ${cfg.apiKey}")
                .post(multipart)
                .build()
            client.newCall(request).execute().use { resp ->
                val text = resp.body?.string().orEmpty()
                if (!resp.isSuccessful) {
                    val snippet = text.take(300).replace("\n", " ")
                    throw AiHttpException(resp.code, "HTTP ${resp.code}：$snippet")
                }
                runCatching {
                    json.parseToJsonElement(text).jsonObject["text"]?.jsonPrimitive?.content
                }.getOrNull() ?: throw AiHttpException(resp.code, "响应缺少 text 字段")
            }
        }
    }

    /**
     * 小米 MiMo ASR（官方协议）：POST {base}/chat/completions，
     * model=mimo-v2.5-asr，音频以 base64 data URL 放在 user 消息的 input_audio 里，
     * 响应 choices[0].message.content 为识别文本。配置了非 asr 模型时自动回退 mimo-v2.5-asr。
     */
    private fun transcribeMimo(base: String, key: String, model: String, wav: ByteArray): String {
        val asrModel = if (model.contains("asr", ignoreCase = true)) model else "mimo-v2.5-asr"
        val b64 = android.util.Base64.encodeToString(wav, android.util.Base64.NO_WRAP)
        val body = buildJsonObject {
            put("model", asrModel)
            put("messages", buildJsonArray {
                add(buildJsonObject {
                    put("role", "user")
                    put("content", buildJsonArray {
                        add(buildJsonObject {
                            put("type", "input_audio")
                            put("input_audio", buildJsonObject {
                                put("data", "data:audio/wav;base64,$b64")
                                put("format", "wav")
                            })
                        })
                    })
                })
            })
            put("asr_options", buildJsonObject { put("language", "auto") })
        }
        val request = Request.Builder()
            .url("$base/chat/completions")
            .header("Authorization", "Bearer $key")
            .post(body.toString().toRequestBody(JSON_MEDIA))
            .build()
        return client.newCall(request).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) {
                val snippet = text.take(300).replace("\n", " ")
                throw AiHttpException(resp.code, "HTTP ${resp.code}：$snippet")
            }
            val content = runCatching {
                json.parseToJsonElement(text).jsonObject["choices"]?.jsonArray?.firstOrNull()
                    ?.jsonObject?.get("message")?.jsonObject?.get("content")?.jsonPrimitive?.content
            }.getOrNull()
            content?.takeIf { it.isNotBlank() }
                ?: throw AiHttpException(resp.code, "响应缺少识别文本（确认模型为 mimo-v2.5-asr）")
        }
    }

    // ------------------------------------------------------------------
    // Audio Speech（TTS）
    // ------------------------------------------------------------------

    /** 合成语音，返回音频字节（wav/mp3 等）。小米 MiMo 走 chat+audio，其余走标准 /audio/speech */
    suspend fun synthesize(cfg: ServiceConfig, text: String): ByteArray = withContext(Dispatchers.IO) {
        ensureConfigured(cfg)
        val base = cfg.resolveBaseUrl()
        if (isMimo(base)) {
            synthesizeMimo(base, cfg.apiKey, cfg.model, text)
        } else {
            val body = buildJsonObject {
                put("model", cfg.model)
                put("input", text)
            }
            val request = Request.Builder()
                .url("$base/audio/speech")
                .header("Authorization", "Bearer ${cfg.apiKey}")
                .post(body.toString().toRequestBody(JSON_MEDIA))
                .build()
            client.newCall(request).execute().use { resp ->
                val bytes = resp.body?.bytes() ?: ByteArray(0)
                if (!resp.isSuccessful) {
                    val snippet = String(bytes, Charsets.UTF_8).take(300).replace("\n", " ")
                    throw AiHttpException(resp.code, "HTTP ${resp.code}：$snippet")
                }
                if (bytes.isEmpty()) throw AiHttpException(resp.code, "响应为空")
                bytes
            }
        }
    }

    /**
     * 小米 MiMo TTS（官方协议）：POST {base}/chat/completions，
     * body 带 audio.format=wav 与一条 role=assistant 的合成文本消息，
     * 响应 JSON 的 audio.data 为 base64 编码的 WAV。模型需 mimo-v2.5-tts 系列；
     * 配置了非 tts 模型时自动回退 mimo-v2.5-tts（TTS 专用），保证可用。
     */
    private fun synthesizeMimo(base: String, key: String, model: String, text: String): ByteArray {
        val ttsModel = if (model.contains("tts", ignoreCase = true)) model else "mimo-v2.5-tts"
        val body = buildJsonObject {
            put("model", ttsModel)
            put("messages", buildJsonArray {
                add(buildJsonObject { put("role", "user"); put("content", text) })
                add(buildJsonObject { put("role", "assistant"); put("content", text) })
            })
            put("audio", buildJsonObject { put("format", "wav") })
        }
        val request = Request.Builder()
            .url("$base/chat/completions")
            .header("Authorization", "Bearer $key")
            .post(body.toString().toRequestBody(JSON_MEDIA))
            .build()
        return client.newCall(request).execute().use { resp ->
            val text2 = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) {
                val snippet = text2.take(300).replace("\n", " ")
                throw AiHttpException(resp.code, "HTTP ${resp.code}：$snippet")
            }
            val b64 = runCatching {
                json.parseToJsonElement(text2).jsonObject["choices"]?.jsonArray?.firstOrNull()
                    ?.jsonObject?.get("message")?.jsonObject?.get("audio")?.jsonObject?.get("data")?.jsonPrimitive?.content
            }.getOrNull()
            if (b64.isNullOrBlank()) {
                throw AiHttpException(resp.code, "响应缺少 audio.data（确认已用 mimo-v2.5-tts 模型）")
            }
            android.util.Base64.decode(b64, android.util.Base64.DEFAULT)
        }
    }

    private fun isMimo(base: String) = base.contains("xiaomimimo.com", ignoreCase = true)

    // ------------------------------------------------------------------
    // 可用性测试（§2.3.4）
    // ------------------------------------------------------------------

    /** LLM 测试：最小 chat（"ping", max_tokens=8），返回耗时 ms */
    suspend fun testChat(cfg: ServiceConfig): String = withContext(Dispatchers.IO) {
        ensureConfigured(cfg)
        val start = System.currentTimeMillis()
        val base = cfg.resolveBaseUrl()
        val body = buildJsonObject {
            put("model", cfg.model)
            put("max_tokens", 8)
            put("messages", buildJsonArray {
                add(buildJsonObject { put("role", "user"); put("content", "ping") })
            })
        }
        val request = Request.Builder()
            .url("$base/chat/completions")
            .header("Authorization", "Bearer ${cfg.apiKey}")
            .post(body.toString().toRequestBody(JSON_MEDIA))
            .build()
        client.newCall(request).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (!resp.isSuccessful) {
                val snippet = text.take(200).replace("\n", " ")
                throw AiHttpException(resp.code, "HTTP ${resp.code}：$snippet")
            }
            "成功，耗时 ${System.currentTimeMillis() - start} ms"
        }
    }

    /** ASR 测试：上传一段约 1s 的音频 */
    suspend fun testAsr(cfg: ServiceConfig, wav: ByteArray): String {
        val start = System.currentTimeMillis()
        val text = transcribe(cfg, wav)
        val shown = if (text.isBlank()) "(空)" else text
        return "成功，返回「${shown.take(20)}」，耗时 ${System.currentTimeMillis() - start} ms"
    }

    /** TTS 测试：合成"测试"并返回音频字节（由调用方播放） */
    suspend fun testTts(cfg: ServiceConfig): ByteArray = synthesize(cfg, "测试")

    // ------------------------------------------------------------------

    private fun ensureConfigured(cfg: ServiceConfig, requireModel: Boolean = true) {
        if (cfg.resolveBaseUrl().isEmpty()) throw AiHttpException(-1, "Base URL 未配置")
        if (cfg.apiKey.isBlank()) throw AiHttpException(-1, "API Key 未配置")
        if (requireModel && cfg.model.isBlank()) throw AiHttpException(-1, "模型名未配置")
    }

    /** 网络层异常 → 用户可读消息（不含敏感信息） */
    fun readableError(e: Throwable): String = when (e) {
        is AiHttpException -> e.message ?: "未知错误"
        is IOException -> when {
            e.message?.contains("timeout", true) == true -> "网络超时"
            e.message?.contains("Failed to connect", true) == true -> "无法连接服务器（检查 Base URL/网络）"
            e.message?.contains("Unable to resolve", true) == true -> "域名解析失败（检查 Base URL）"
            else -> "网络错误：${e.message ?: e.javaClass.simpleName}"
        }
        else -> "${e.javaClass.simpleName}: ${e.message}"
    }
}
