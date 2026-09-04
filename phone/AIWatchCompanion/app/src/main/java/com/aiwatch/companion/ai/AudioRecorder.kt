package com.aiwatch.companion.ai

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream

class RecordingCanceledException : Exception("录音已取消")

/**
 * 录音器：16 kHz / 16 bit / 单声道，能量 VAD：
 * - 前置 500ms 采噪声底，阈值 = max(900, 底噪×2.5)
 * - 检测到说话后静音 1.2s 自动结束
 * - 最长 15s
 * 输出整段 WAV 字节（供 ASR 模型服务上传）。
 */
@SuppressLint("MissingPermission") // 调用方保证已授予 RECORD_AUDIO
class AudioRecorder(
    private val sampleRate: Int = 16_000,
    private val maxDurationMs: Long = 15_000,
    private val silenceStopMs: Long = 1_200,
    private val onAmplitude: ((Float) -> Unit)? = null, // 0..1 归一化电平（UI 实时反馈）
) {

    @Volatile
    private var canceled = false

    fun cancel() {
        canceled = true
    }

    /** 录制并返回 WAV；用户取消抛 [RecordingCanceledException] */
    suspend fun record(): ByteArray = withContext(Dispatchers.IO) {
        canceled = false
        val minBuf = AudioRecord.getMinBufferSize(sampleRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val bufferSize = maxOf(minBuf, sampleRate) // ≥1s 缓冲
        val recorder = try {
            AudioRecord(
                MediaRecorder.AudioSource.MIC,
                sampleRate,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                bufferSize,
            )
        } catch (e: SecurityException) {
            throw IllegalStateException("缺少录音权限，请到「设置-权限」授予麦克风", e)
        } catch (e: Exception) {
            throw IllegalStateException("录音器初始化失败：${e.message}", e)
        }
        if (recorder.state != AudioRecord.STATE_INITIALIZED) {
            recorder.release()
            throw IllegalStateException("麦克风不可用（被其他应用占用或权限未授予）")
        }

        val pcm = ByteArrayOutputStream()
        val chunk = ShortArray(sampleRate / 10) // 100ms
        var speechStarted = false
        var speechRmsPeak = 0.0
        var speechFrames = 0
        var noiseSum = 0.0
        var noiseFrames = 0

        try {
            recorder.startRecording()
            val startAt = System.currentTimeMillis()
            var silenceSince = 0L
            while (true) {
                currentCoroutineContext().ensureActive()
                if (canceled) throw RecordingCanceledException()
                val elapsed = System.currentTimeMillis() - startAt
                if (elapsed >= maxDurationMs) break

                val n = recorder.read(chunk, 0, chunk.size)
                if (n <= 0) continue
                var sum = 0.0
                for (i in 0 until n) {
                    val v = chunk[i].toDouble()
                    sum += v * v
                    // Short → little-endian bytes
                    pcm.write(chunk[i].toInt() and 0xFF)
                    pcm.write((chunk[i].toInt() shr 8) and 0xFF)
                }
                val rms = kotlin.math.sqrt(sum / n)
                val level = (rms / 3000.0).coerceIn(0.0, 1.0)
                onAmplitude?.invoke(level.toFloat())

                if (elapsed < NOISE_FLOOR_MS) {
                    noiseSum += rms; noiseFrames++
                    continue
                }
                if (noiseFrames > 0) {
                    val floor = noiseSum / noiseFrames
                    speechRmsPeak = maxOf(900.0, floor * 2.5)
                    noiseFrames = -1 // 标记已定阈值
                }
                if (rms >= speechRmsPeak) {
                    speechStarted = true
                    speechFrames++
                    silenceSince = 0
                } else if (speechStarted) {
                    if (silenceSince == 0L) silenceSince = System.currentTimeMillis()
                    else if (System.currentTimeMillis() - silenceSince >= silenceStopMs) break
                }
            }
        } finally {
            runCatching { recorder.stop() }
            recorder.release()
        }

        if (speechFrames < 3) throw IllegalStateException("未检测到语音，请靠近手表端再说一次")
        buildWav(pcm.toByteArray(), sampleRate, 1, 16)
    }

    companion object {
        private const val NOISE_FLOOR_MS = 500L

        /** PCM(LE) → WAV 容器 */
        fun buildWav(pcm: ByteArray, sampleRate: Int, channels: Int, bitsPerSample: Int): ByteArray {
            val byteRate = sampleRate * channels * bitsPerSample / 8
            val blockAlign = channels * bitsPerSample / 8
            val out = ByteArrayOutputStream(44 + pcm.size)
            fun le32(v: Int) {
                out.write(v and 0xFF); out.write((v shr 8) and 0xFF)
                out.write((v shr 16) and 0xFF); out.write((v shr 24) and 0xFF)
            }
            fun le16(v: Int) {
                out.write(v and 0xFF); out.write((v shr 8) and 0xFF)
            }
            out.write("RIFF".toByteArray()); le32(36 + pcm.size)
            out.write("WAVE".toByteArray())
            out.write("fmt ".toByteArray()); le32(16)
            le16(1) // PCM
            le16(channels); le32(sampleRate); le32(byteRate); le16(blockAlign); le16(bitsPerSample)
            out.write("data".toByteArray()); le32(pcm.size)
            out.write(pcm)
            return out.toByteArray()
        }

        /** 录制约 1s 环境音，用于 ASR 可用性测试 */
        suspend fun recordShortTest(): ByteArray = withContext(Dispatchers.IO) {
            val sampleRate = 16_000
            val recorder = try {
                AudioRecord(
                    MediaRecorder.AudioSource.MIC, sampleRate,
                    AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
                    maxOf(sampleRate, AudioRecord.getMinBufferSize(sampleRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)),
                )
            } catch (e: SecurityException) {
                throw IllegalStateException("缺少录音权限，请到「设置-权限」授予麦克风", e)
            }
            if (recorder.state != AudioRecord.STATE_INITIALIZED) {
                recorder.release()
                throw IllegalStateException("麦克风不可用（被其他应用占用或权限未授予）")
            }
            val pcm = ByteArrayOutputStream()
            val chunk = ShortArray(sampleRate / 10)
            try {
                recorder.startRecording()
                val deadline = System.currentTimeMillis() + 1_000
                while (System.currentTimeMillis() < deadline) {
                    val n = recorder.read(chunk, 0, chunk.size)
                    if (n > 0) {
                        for (i in 0 until n) {
                            pcm.write(chunk[i].toInt() and 0xFF)
                            pcm.write((chunk[i].toInt() shr 8) and 0xFF)
                        }
                    }
                }
            } finally {
                runCatching { recorder.stop() }
                recorder.release()
            }
            buildWav(pcm.toByteArray(), sampleRate, 1, 16)
        }
    }
}
