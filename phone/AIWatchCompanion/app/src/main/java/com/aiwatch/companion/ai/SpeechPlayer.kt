package com.aiwatch.companion.ai

import android.content.Context
import android.media.MediaPlayer
import android.speech.tts.TextToSpeech
import java.io.File
import java.util.Locale

/**
 * 手机端语音播报：
 * - 系统自带：TextToSpeech 本地合成播放
 * - 模型服务：AiHttp.synthesize 的音频字节经 MediaPlayer 播放
 * TTS 失败只记日志，绝不影响 AI_TEXT 下发（§2.3.5）。
 */
object SpeechPlayer {

    private var tts: TextToSpeech? = null
    private var ttsReady = false
    private var player: MediaPlayer? = null

    /** 懒初始化系统 TTS（回调异步完成，未就绪时 speak 会静默跳过） */
    fun ensureTts(context: Context) {
        if (tts != null) return
        tts = TextToSpeech(context.applicationContext) { status ->
            ttsReady = status == TextToSpeech.SUCCESS
            if (ttsReady) {
                runCatching { tts?.language = Locale.SIMPLIFIED_CHINESE }
            }
        }
    }

    /** 系统自带播报 */
    fun speak(text: String) {
        val engine = tts ?: return
        if (!ttsReady) return
        runCatching {
            engine.speak(text, TextToSpeech.QUEUE_FLUSH, null, "ai_reply_${System.currentTimeMillis()}")
        }
    }

    /** 模型服务播报：音频字节写入临时文件 → MediaPlayer 播放 */
    fun playAudio(context: Context, audio: ByteArray) {
        stopAudio()
        runCatching {
            val dir = File(context.cacheDir, "tts")
            dir.mkdirs()
            val file = File(dir, "reply_${System.currentTimeMillis()}.audio")
            file.writeBytes(audio)
            player = MediaPlayer().apply {
                setDataSource(file.absolutePath)
                setOnCompletionListener {
                    runCatching { it.release() }
                    player = null
                    file.delete()
                }
                prepare()
                start()
            }
        }.onFailure {
            player = null
        }
    }

    fun stopAudio() {
        runCatching {
            player?.stop()
            player?.release()
        }
        player = null
    }

    /** 停止一切播报（录音前调用，避免抢麦克风/混音） */
    fun stopAll() {
        runCatching { tts?.stop() }
        stopAudio()
    }
}
