package com.aiwatch.companion.ai

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

class SystemAsrUnavailableException(message: String) : Exception(message)
class AsrNoMatchException(message: String = "未识别到语音") : Exception(message)

/**
 * 系统自带识别器（SpeechRecognizer）挂起封装。
 * SpeechRecognizer 依赖主线程 Looper，全程在 Main 上创建与销毁。
 */
class SystemAsr(private val context: Context) {

    /** 识别语音并返回文本（内部自己录音，系统处理 VAD） */
    suspend fun recognize(): String = withContext(Dispatchers.Main) {
        if (!SpeechRecognizer.isRecognitionAvailable(context)) {
            throw SystemAsrUnavailableException("系统语音识别不可用（可改用 ASR 模型服务）")
        }
        val sr = SpeechRecognizer.createSpeechRecognizer(context)
        try {
            val deferred = CompletableDeferred<String>()
            sr.setRecognitionListener(object : RecognitionListener {
                override fun onReadyForSpeech(params: Bundle?) {}
                override fun onBeginningOfSpeech() {}
                override fun onRmsChanged(rmsdB: Float) {}
                override fun onBufferReceived(buffer: ByteArray?) {}
                override fun onEndOfSpeech() {}
                override fun onError(error: Int) {
                    deferred.completeExceptionally(
                        when (error) {
                            SpeechRecognizer.ERROR_NO_MATCH, SpeechRecognizer.ERROR_SPEECH_TIMEOUT ->
                                AsrNoMatchException()
                            SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS ->
                                IllegalStateException("缺少录音权限")
                            SpeechRecognizer.ERROR_NETWORK, SpeechRecognizer.ERROR_NETWORK_TIMEOUT ->
                                IllegalStateException("识别服务网络错误")
                            SpeechRecognizer.ERROR_RECOGNIZER_BUSY ->
                                IllegalStateException("识别器忙，请稍后重试")
                            else -> IllegalStateException("识别错误 code=$error")
                        },
                    )
                }

                override fun onResults(results: Bundle?) {
                    val text = results
                        ?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
                        ?.firstOrNull().orEmpty()
                    if (text.isBlank()) deferred.completeExceptionally(AsrNoMatchException())
                    else deferred.complete(text)
                }

                override fun onPartialResults(partialResults: Bundle?) {}
                override fun onEvent(eventType: Int, params: Bundle?) {}
            })

            val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
                putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
                putExtra(RecognizerIntent.EXTRA_LANGUAGE, "zh-CN")
                putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, false)
                putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 1)
            }
            sr.startListening(intent)

            // 录音+识别整体限时（§2.1 录音 ≤15s，放宽到 18s 给系统引擎余量）
            val result = withTimeoutOrNull(18_000) { deferred.await() }
            if (result == null) {
                throw AsrNoMatchException("识别超时，请再说一次")
            } else {
                result
            }
        } finally {
            runCatching { sr.stopListening() }
            runCatching { sr.destroy() }
        }
    }
}
