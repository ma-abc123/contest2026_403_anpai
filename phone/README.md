# AI Watch 手机端配套应用（AIWatchCompanion）

手表端 AI Watch 的手机伴侣 App（v3.0.0）：通过 BLE GATT 与手表（广播名
`AI-Watch-403`）通信，是"AI 语音闭环、提醒/通知下发、运动与跌倒事件接收"
的另一半。

- **技术栈**：Kotlin + Jetpack Compose（Material 3），minSdk 26 / targetSdk 36，
  Gradle 8.14.3（AGP 8.11.1 / Kotlin 2.2.0）
- **功能**：扫描/重连手表、时间同步、提醒/通知下发与到点条幅、运动数据
  （步数/活动状态）与疑似跌倒事件接收（0x11 闹钟级提醒 + 前台置顶）、
  原始运动流 JSONL 存档（0x12）、手表一键 AI 语音闭环（录音 → ASR →
  LLM → AI_TEXT/AI_TIMER/提醒下发）
- **协议**：与固件 `app/ai_watch/ai_watch_ble.h` 同源的自定义 GATT 协议，
  帧格式见 `ble/BleProto.kt`；需求与验收记录见仓内
  `docs/development/手机端*.md`

## 构建

```bash
cd phone/AIWatchCompanion
./gradlew assembleDebug     # → app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

注意事项：

- **JDK 17**：`gradle.properties` 中 `org.gradle.java.home` 固定了开发机
  JDK 17 路径，换机器构建请删除或修改该行（系统 Java 8 无法运行现代 AGP）
- **Gradle 镜像**：`gradle-wrapper.properties` 的 distributionUrl 指向腾讯云
  镜像（国内下载加速），可按需改回官方源
- **签名**：出于安全考虑，发布签名私钥（`keystore/`）与本机 SDK 路径
  （`local.properties`）未随仓提交；debug 构建不受影响，assembleRelease
  需自备签名配置
- LLM / ASR / TTS 服务地址与 API Key 在 App 设置页配置（OpenAI 兼容协议），
  运行时存储，源码中无硬编码密钥

## 目录速览

```
app/src/main/java/com/aiwatch/companion/
  ├─ ble/    # GATT 协议、连接管理、前台服务
  ├─ ai/     # ASR/LLM/TTS HTTP 客户端、会话管理、录音与播放
  ├─ data/   # 提醒/跌倒事件/运动流/设置 持久化
  └─ ui/     # Compose 界面（仪表盘/运动/提醒/AI/日志/设置 六页）
MANIFEST.md  # v1→v3 迭代记录（功能与验收清单）
```
