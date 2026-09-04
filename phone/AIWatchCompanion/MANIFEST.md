# MANIFEST — AI Watch 手机配套应用开发

> 本清单记录本次任务产生的全部文件与系统副作用，确保环境可还原。
> v1 生成时间：2026-08-29；v2（AI 闭环）更新：2026-08-31；v3（运动数据）更新：2026-08-31

## 〇、版本

- v1.0.0：扫描/重连/TimeSync/提醒/DataUpload 仪表盘
- v2.0.0：手表 AI 触发(f3 0x10) → 录音 → ASR → LLM(强制 JSON) → AI_TEXT(0x04)/AI_TIMER(0x05)/提醒/通知 下发闭环；LLM/ASR/TTS 三组独立 OpenAI 兼容服务配置（含模型列表拉取与可用性测试）；AI 会话页与历史；后台触发高优先级通知点开即录。
- **v3.0.0（当前）**：运动数据上报与疑似跌倒接收（固件 4.8.0）。0x05 今日步数 / 0x06 活动状态 / 0x11 疑似跌倒（event=1 闹钟级通知+前台置顶弹窗，2/3 普通通知）/ 0x12 原始运动流 JSONL 存档（约 8Hz，Channel 队列不阻塞 BLE）；新增运动页与仪表盘运动区块；底部导航六页。

## 一、项目主目录（本次核心交付物）

`C:\Users\abc12\Desktop\work\AIWatchCompanion\`

| 路径 | 说明 |
|---|---|
| `settings.gradle.kts` / `build.gradle.kts` / `gradle.properties` | Gradle 构建配置（AGP 8.11.1 / Kotlin 2.2.0 / JDK 17） |
| `local.properties` | 本机 SDK 路径（勿提交） |
| `gradlew` / `gradlew.bat` / `gradle/wrapper/*` | Gradle 8.14.3 wrapper（distributionUrl 已指向腾讯镜像以加速国内下载） |
| `keystore/aiwatch-release.keystore` | release 签名密钥（口令 `aiwatch2026`，别名 `aiwatch`） |
| `app/src/main/AndroidManifest.xml` | 权限（BLUETOOTH_SCAN neverForLocation / CONNECT / FGS connectedDevice） |
| `app/src/main/java/com/aiwatch/companion/ble/BleProto.kt` | 协议编解码（小端序、帧构造/解析、错误码） |
| `app/src/main/java/com/aiwatch/companion/ble/WatchBleManager.kt` | 连接状态机（扫描/MTU/订阅/校时/指数退避重连/串行写队列） |
| `app/src/main/java/com/aiwatch/companion/ble/WatchForegroundService.kt` | 前台服务保活 |
| `app/src/main/java/com/aiwatch/companion/ble/WatchLog.kt` | 联调日志（hex 环形缓冲） |
| `app/src/main/java/com/aiwatch/companion/data/ReminderRepository.kt` | 提醒本地存储 + 发送记录（JSON） |
| `app/src/main/java/com/aiwatch/companion/data/SettingsRepository.kt` | 设置（DataStore） |
| `app/src/main/java/com/aiwatch/companion/ai/**` | **v2 新增**：AiConfig（LLM/ASR/TTS 三组配置+测试状态）、AiHttp（OpenAI 兼容：chat/models/transcriptions/speech）、AudioRecorder（16k/16bit 录音+VAD）、SystemAsr、SpeechPlayer、AiSessionManager（会话状态机/触发通知/历史持久化） |
| `app/src/main/java/com/aiwatch/companion/data/FallRepository.kt` | **v3 新增**：跌倒事件记录（JSON，保留 100 条） |
| `app/src/main/java/com/aiwatch/companion/data/MotionRecorder.kt` | **v3 新增**：原始运动流 JSONL 存档（Channel 队列 + IO 协程，不阻塞 BLE 回调） |
| `app/src/main/java/com/aiwatch/companion/ui/**` | Compose Material 3 UI（**六页**：仪表盘/运动/AI/提醒/日志/设置；运动页 + 跌倒置顶弹窗 + 仪表盘运动区块为 v3 新增） |
| `app/src/main/res/**` | 主题、图标（自适应启动图标 + 状态栏图标） |
| `app/build/outputs/apk/debug/app-debug.apk` | 调试包 v3.0.0（约 18MB，可直接安装联调） |
| `app/build/outputs/apk/release/app-release.apk` | 已签名发布包 v3.0.0（约 12MB，v2 签名，可直接安装） |
| `.gitignore` | 忽略 build/ 与 local.properties |

原始需求文件（工作区根）**均未被修改**：`开发规范.txt`、`手机端AI闭环需求-2026-08-30.md`、`手机端运动数据上报需求-2026-08-31.md`。

## 二、工作区外的副作用（均为标准开发缓存，可用即删）

| 位置 | 内容 | 还原方式 |
|---|---|---|
| `C:\Users\abc12\.gradle\wrapper\dists\gradle-8.14.3-bin\` | Gradle 8.14.3 发行版（约 140MB，wrapper 自动下载） | 删除该目录即可 |
| `C:\Users\abc12\.gradle\caches\` | AGP/Kotlin/AndroidX 依赖缓存 | 删除 `caches` 目录即可 |
| `C:\Users\abc12\.gradle\daemon\` | Gradle 守护进程（空闲 3h 自动退出） | `gradlew --stop` 或等待自动退出 |

> 未安装任何系统级软件、未改动注册表、未触碰用户个人目录。

## 三、一键还原

1. 删除 `C:\Users\abc12\Desktop\work\AIWatchCompanion\` 整个目录；
2. （可选）删除 `C:\Users\abc12\.gradle\` 下的 `wrapper/dists/gradle-8.14.3-bin`、`caches`、`daemon`。

## 四、构建/运行

```
cd C:\Users\abc12\Desktop\work\AIWatchCompanion
gradlew.bat assembleDebug        # 调试包
gradlew.bat assembleRelease      # 签名发布包
adb install -r app\build\outputs\apk\release\app-release.apk
```
