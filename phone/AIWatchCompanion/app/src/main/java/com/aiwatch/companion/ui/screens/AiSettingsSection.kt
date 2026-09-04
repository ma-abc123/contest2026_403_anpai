package com.aiwatch.companion.ui.screens

import android.Manifest
import android.content.pm.PackageManager
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Bolt
import androidx.compose.material.icons.outlined.CheckCircle
import androidx.compose.material.icons.outlined.ErrorOutline
import androidx.compose.material.icons.outlined.GraphicEq
import androidx.compose.material.icons.outlined.Psychology
import androidx.compose.material.icons.outlined.RecordVoiceOver
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import com.aiwatch.companion.WatchApp
import com.aiwatch.companion.ai.AiConfig
import com.aiwatch.companion.ai.AiHttp
import com.aiwatch.companion.ai.AudioRecorder
import com.aiwatch.companion.ai.BaseUrlPreset
import com.aiwatch.companion.ai.ServiceConfig
import com.aiwatch.companion.ai.ServiceMode
import com.aiwatch.companion.ai.SpeechPlayer
import kotlinx.coroutines.launch

/**
 * 设置页「AI 服务」区块：LLM / ASR / TTS 三组独立配置
 * （Base URL 预置列表 + 自定义、Key 掩码、刷新模型列表、测试）。
 * Key 只保存在本机 DataStore，输入掩码显示，绝不写入日志。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AiSettingsSection(app: WatchApp) {
    val scope = rememberCoroutineScope()
    val context = LocalContext.current

    // 本地编辑态：首次载入后以本地为准，修改即时持久化
    var local by remember { mutableStateOf<AiConfig?>(null) }
    LaunchedEffect(Unit) {
        if (local == null) local = app.ai.configRepo.current()
    }
    val cfg = local ?: return

    fun commit(mutate: (AiConfig) -> AiConfig) {
        val next = mutate(local!!)
        local = next
        scope.launch { app.ai.configRepo.update { mutate(it) } }
    }

    fun commitService(get: (AiConfig) -> ServiceConfig, set: (AiConfig, ServiceConfig) -> AiConfig, mutate: (ServiceConfig) -> ServiceConfig) {
        commit { c -> set(c, mutate(get(c))) }
    }

    var llmTesting by remember { mutableStateOf(false) }
    var asrTesting by remember { mutableStateOf(false) }
    var ttsTesting by remember { mutableStateOf(false) }

    Column {
        Text("AI 服务", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(8.dp))

        // ---------- LLM（必配） ----------
        ServiceCard(
            title = "LLM 语言模型（必配）",
            icon = Icons.Outlined.Psychology,
            desc = "语音理解的模型服务（OpenAI 兼容）。未配置或测试未通过时，AI 会话不可用",
        ) {
            ServiceFields(
                cfg = cfg.llm,
                mutate = { commitService(AiConfig::llm, { c, s -> c.copy(llm = s) }, it) },
            )
            Spacer(Modifier.height(8.dp))
            TestRow(
                running = llmTesting,
                testOk = cfg.llmTestOk,
                testNote = cfg.llmTestNote,
                onTest = {
                    llmTesting = true
                    scope.launch {
                        val note = runCatching { AiHttp.testChat(cfg.llm) }
                            .fold(onSuccess = { it }, onFailure = { AiHttp.readableError(it) })
                        app.ai.configRepo.setLlmTest(note.startsWith("成功"), note)
                        local = app.ai.configRepo.current()
                        llmTesting = false
                    }
                },
            )
        }

        Spacer(Modifier.height(12.dp))

        // ---------- ASR ----------
        ServiceCard(
            title = "语音识别 ASR",
            icon = Icons.Outlined.GraphicEq,
            desc = "默认系统自带识别器（零配置）；模型服务按 16kHz/16bit/单声道整段上传（小米 MiMo 自动使用 mimo-v2.5-asr）",
        ) {
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                SegmentedButton(
                    selected = cfg.asrModeEnum == ServiceMode.SYSTEM,
                    onClick = { commit { it.copy(asrMode = ServiceMode.SYSTEM.ordinal) } },
                    shape = SegmentedButtonDefaults.itemShape(index = 0, count = 2),
                ) { Text("系统自带") }
                SegmentedButton(
                    selected = cfg.asrModeEnum == ServiceMode.MODEL,
                    onClick = { commit { it.copy(asrMode = ServiceMode.MODEL.ordinal) } },
                    shape = SegmentedButtonDefaults.itemShape(index = 1, count = 2),
                ) { Text("模型服务") }
            }
            if (cfg.asrModeEnum == ServiceMode.MODEL) {
                Spacer(Modifier.height(10.dp))
                ServiceFields(
                    cfg = cfg.asr,
                    mutate = { commitService(AiConfig::asr, { c, s -> c.copy(asr = s) }, it) },
                )
                Spacer(Modifier.height(8.dp))
                TestRow(
                    running = asrTesting,
                    testOk = cfg.asrTestOk,
                    testNote = cfg.asrTestNote,
                    onTest = {
                        val micOk = ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
                        if (!micOk) {
                            val note = "缺少麦克风权限：请到系统设置授予录音权限"
                            scope.launch {
                                app.ai.configRepo.setAsrTest(false, note)
                                local = app.ai.configRepo.current()
                            }
                        } else {
                            asrTesting = true
                            scope.launch {
                                // 录制约 1s 环境音上传，验证可用性（§2.3.4）
                                val note = runCatching {
                                    val wav = AudioRecorder.recordShortTest()
                                    AiHttp.testAsr(cfg.asr, wav)
                                }.fold(
                                    onSuccess = { it },
                                    onFailure = { e -> AiHttp.readableError(e) },
                                )
                                app.ai.configRepo.setAsrTest(note.startsWith("成功"), note)
                                local = app.ai.configRepo.current()
                                asrTesting = false
                            }
                        }
                    },
                )
            }
        }

        Spacer(Modifier.height(12.dp))

        // ---------- TTS ----------
        ServiceCard(
            title = "语音播报 TTS",
            icon = Icons.Outlined.RecordVoiceOver,
            desc = "回复文本在手机端播报（手表不做音频输出）；TTS 失败不影响文本下发（小米 MiMo 自动使用 mimo-v2.5-tts 模型合成）",
        ) {
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                SegmentedButton(
                    selected = cfg.ttsModeEnum == ServiceMode.SYSTEM,
                    onClick = { commit { it.copy(ttsMode = ServiceMode.SYSTEM.ordinal) } },
                    shape = SegmentedButtonDefaults.itemShape(index = 0, count = 2),
                ) { Text("系统自带") }
                SegmentedButton(
                    selected = cfg.ttsModeEnum == ServiceMode.MODEL,
                    onClick = { commit { it.copy(ttsMode = ServiceMode.MODEL.ordinal) } },
                    shape = SegmentedButtonDefaults.itemShape(index = 1, count = 2),
                ) { Text("模型服务") }
            }
            if (cfg.ttsModeEnum == ServiceMode.MODEL) {
                Spacer(Modifier.height(10.dp))
                ServiceFields(
                    cfg = cfg.tts,
                    mutate = { commitService(AiConfig::tts, { c, s -> c.copy(tts = s) }, it) },
                )
                Spacer(Modifier.height(8.dp))
                TestRow(
                    running = ttsTesting,
                    testOk = cfg.ttsTestOk,
                    testNote = cfg.ttsTestNote,
                    onTest = {
                        ttsTesting = true
                        scope.launch {
                            val note = runCatching {
                                val audio = AiHttp.testTts(cfg.tts)
                                SpeechPlayer.playAudio(app, audio)
                                "成功，已本地播放"
                            }.fold(
                                onSuccess = { it },
                                onFailure = { e -> AiHttp.readableError(e) },
                            )
                            app.ai.configRepo.setTtsTest(note.startsWith("成功"), note)
                            local = app.ai.configRepo.current()
                            ttsTesting = false
                        }
                    },
                )
            }
        }
    }
}

// ---------------------------------------------------------------------

@Composable
private fun ServiceCard(title: String, icon: ImageVector, desc: String, content: @Composable () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(icon, null, tint = MaterialTheme.colorScheme.primary)
                Spacer(Modifier.width(8.dp))
                Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
            }
            Spacer(Modifier.height(4.dp))
            Text(desc, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(10.dp))
            content()
        }
    }
}

/**
 * 服务字段编辑：Base URL 预置选择（含自定义输入）、API Key（掩码）、模型名（可拉取列表 / 手输）。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ServiceFields(
    cfg: ServiceConfig,
    mutate: ((ServiceConfig) -> ServiceConfig) -> Unit,
) {
    var showKey by remember { mutableStateOf(false) }
    var fetching by remember { mutableStateOf(false) }
    var fetchError by remember { mutableStateOf<String?>(null) }
    var fetchedModels by remember { mutableStateOf<List<String>?>(null) }
    val scope = rememberCoroutineScope()

    Text("Base URL", style = MaterialTheme.typography.labelMedium)
    Spacer(Modifier.height(4.dp))
    var urlMenuOpen by remember { mutableStateOf(false) }
    val currentPreset = BaseUrlPreset.byKey(cfg.baseUrlPresetKey)
    ExposedDropdownMenuBox(
        expanded = urlMenuOpen,
        onExpandedChange = { urlMenuOpen = it },
    ) {
        OutlinedTextField(
            value = if (currentPreset.isCustom && cfg.customBaseUrl.isNotBlank()) cfg.customBaseUrl else currentPreset.label,
            onValueChange = {},
            readOnly = true,
            singleLine = true,
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = urlMenuOpen) },
            modifier = Modifier
                .fillMaxWidth()
                .menuAnchor(MenuAnchorType.PrimaryNotEditable),
        )
        ExposedDropdownMenu(
            expanded = urlMenuOpen,
            onDismissRequest = { urlMenuOpen = false },
        ) {
            BaseUrlPreset.ALL.forEach { preset ->
                DropdownMenuItem(
                    text = {
                        Column {
                            Text(preset.label, style = MaterialTheme.typography.bodyMedium)
                            if (preset.url.isNotEmpty()) {
                                Text(
                                    preset.url,
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                    },
                    onClick = {
                        mutate { it.copy(baseUrlPresetKey = preset.key) }
                        urlMenuOpen = false
                    },
                )
            }
        }
    }
    if (cfg.baseUrlPresetKey == BaseUrlPreset.KEY_CUSTOM) {
        Spacer(Modifier.height(6.dp))
        OutlinedTextField(
            value = cfg.customBaseUrl,
            onValueChange = { v -> mutate { it.copy(customBaseUrl = v) } },
            label = { Text("自定义 Base URL（OpenAI 兼容，如 https://host/v1）") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
    } else {
        Text(
            cfg.resolveBaseUrl(),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }

    Spacer(Modifier.height(8.dp))
    OutlinedTextField(
        value = cfg.apiKey,
        onValueChange = { v -> mutate { it.copy(apiKey = v) } },
        label = { Text("API Key") },
        singleLine = true,
        visualTransformation = if (showKey) VisualTransformation.None else PasswordVisualTransformation(),
        trailingIcon = {
            TextButton(onClick = { showKey = !showKey }) { Text(if (showKey) "隐藏" else "显示") }
        },
        modifier = Modifier.fillMaxWidth(),
    )

    Spacer(Modifier.height(8.dp))
    OutlinedTextField(
        value = cfg.model,
        onValueChange = { v -> mutate { it.copy(model = v) } },
        label = { Text("模型名（可手输，或刷新拉取后选择）") },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )

    Spacer(Modifier.height(6.dp))
    Row(verticalAlignment = Alignment.CenterVertically) {
        Button(
            onClick = {
                fetching = true
                fetchError = null
                scope.launch {
                    fetchedModels = try {
                        AiHttp.listModels(cfg)
                    } catch (e: Exception) {
                        fetchError = "拉取失败：" + AiHttp.readableError(e) + "（可手动输入模型名）"
                        null
                    }
                    fetching = false
                }
            },
            enabled = !fetching && cfg.apiKey.isNotBlank() && cfg.resolveBaseUrl().isNotEmpty(),
        ) {
            if (fetching) {
                CircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
                Spacer(Modifier.width(6.dp))
            }
            Text("刷新模型列表")
        }
        fetchError?.let {
            Spacer(Modifier.width(8.dp))
            Text(
                it,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.weight(1f),
            )
        }
    }

    fetchedModels?.let { models ->
        AlertDialog(
            onDismissRequest = { fetchedModels = null },
            title = { Text("选择模型（qwen / mimo 置顶）") },
            text = {
                if (models.isEmpty()) {
                    Text("列表为空，可手动输入模型名")
                } else {
                    LazyColumn(modifier = Modifier.height(360.dp)) {
                        items(models.size) { i ->
                            val m = models[i]
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .clickable {
                                        mutate { it.copy(model = m) }
                                        fetchedModels = null
                                    }
                                    .padding(vertical = 6.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                RadioButton(selected = cfg.model == m, onClick = null)
                                Spacer(Modifier.width(8.dp))
                                Text(m, style = MaterialTheme.typography.bodySmall)
                            }
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { fetchedModels = null }) { Text("关闭") } },
        )
    }
}

// ---------------------------------------------------------------------

@Composable
private fun TestRow(running: Boolean, testOk: Boolean?, testNote: String, onTest: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Button(onClick = onTest, enabled = !running) {
            Icon(Icons.Outlined.Bolt, null, modifier = Modifier.size(16.dp))
            Spacer(Modifier.width(4.dp))
            Text(if (running) "测试中…" else "测试")
        }
        Spacer(Modifier.width(10.dp))
        when {
            running -> CircularProgressIndicator(modifier = Modifier.size(18.dp), strokeWidth = 2.dp)
            testOk == true -> {
                Icon(Icons.Outlined.CheckCircle, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(16.dp))
                Spacer(Modifier.width(4.dp))
                Text(testNote, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.primary)
            }
            testOk == false -> {
                Icon(Icons.Outlined.ErrorOutline, null, tint = MaterialTheme.colorScheme.error, modifier = Modifier.size(16.dp))
                Spacer(Modifier.width(4.dp))
                Text(testNote, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
            }
            else -> Text("未测试", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.outline)
        }
    }
}
