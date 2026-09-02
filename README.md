# AI Watch —— 基于 openvela 的多功能 AI 腕上助手

> openvela AI 硬件开发者大赛 · 队伍 403（anpai）· 硬件：立创·黄山派（SF32LB52）

## 一、作品简介

**AI Watch** 是一台跑着 openvela（NuttX 内核）的智能腕表：在黄山派开发板的屏幕、触摸、按键之上，实现了一套完整的手表应用平台与手机互联协议，并围绕"运动与安全"做出了核心亮点——**基于板载六轴传感器的疑似跌倒检测**：检测到疑似跌倒后全屏 15 秒倒计时，可按键/触摸取消，超时自动上报手机；检测算法（合加速度冲击、自由落体、姿态翻转、冲击后静止四要素）先经 6 组真实测试集离线定标再上板，当前 **误报 0 / 漏报 0**（详见开发日志 2026-09-02）。

功能一览（截至 v4.8.3）：

- **表盘与应用平台**：翻页表盘、六边形应用菜单（hex menu）、三主题、手势导航 + KEY2
- **运动与跌倒检测**：计步/距离、静止/步行/跑步分类、疑似跌倒检测与告警流程、原始数据采集模式（串口全量 CSV + BLE 抽样流）
- **手机互联（BLE GATT）**：时间同步、提醒/通知下发与到点条幅、传感器数据与事件上报、AI 语音闭环触发
- **AI 助手**：手表一键触发 → 手机 ASR/LLM → 结果回显，可落地为提醒/倒计时
- **实用工具**：倒计时、提醒（含到点条幅）、DHT22 温湿度、心率血氧页（外接传感器）

## 二、选题方向

**AI 硬件产品创新**（手表应用形态，自定方向）。选择黄山派 SF32LB52：屏幕/触摸/按键/蓝牙/板载运动传感器齐备，适合腕上形态；openvela 的 LVGL + NuttX 栈成熟，可以把精力放在"跌倒检测这类真实痛点 + AI 手机协同"的产品逻辑上。

## 三、目录结构

```
app/ai_watch/      # AI Watch 主应用（LVGL UI、应用框架、运动服务、BLE 协议层）
  ├─ ai_watch_main.c      # 应用框架、页面、按键/触摸、跌倒告警 UI
  ├─ ai_watch_motion.c/h  # 后台运动服务：采样线程、计步/活动/跌倒算法、数据采集
  ├─ ai_watch_ble.c/h     # 协议层：TimeSync/命令解析/提醒存储/上行队列
  ├─ ai_watch_icons.*     # 图标资源（用户图标集生成）
  └─ fonts/, icons/       # CJK 字体与图标源
board/ai_watch/    # 板级 defconfig（映射到 vendor/openvela/boards/contest2026_403_ai_watch）
docs/development/  # 开发日志（按日期）、手机端需求文档、排查文档
tools/             # fall_threshold_analysis.py：跌倒阈值离线定标脚本
logs/              # AI Coding 日志（按官方手册导出提交）
```

> 代码经 manifest `<linkfile>` 映射进 openvela 编译树（`packages/demos/contest2026_403_ai_watch/` 等），公共仓库零改动；仅 vendor/sifli 下有少量板级/桥接层修改（LSM6DS3 板级注册、BLE 桥接 indication 兼容）。

## 四、运行方式

### 1. 拉取工程

```bash
repo init -u https://github.com/open-vela/contest2026_403_anpai \
  -b dev-ai-contest-2026 -m contest2026_403_anpai.xml
repo sync -c -j8
```

### 2. 编译（openvela 工作区根目录）

```bash
cmake -B cmake_out/contest2026_403_ai_watch -DNUTTX_BUILD=y \
  -DBOARD_CONFIG=../vendor/openvela/boards/contest2026_403_ai_watch/configs/ai_watch \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=return-mismatch -D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS"
cmake --build cmake_out/contest2026_403_ai_watch --parallel 4
# 产物：cmake_out/contest2026_403_ai_watch/nuttx.bin
```

（等价于 `./build.sh contest2026_403_anpai/board/ai_watch/configs/ai_watch`；应用经 `CONFIG_EXAMPLES_AI_WATCH=y` 启用，已写入 defconfig。）

### 3. 烧录与运行

```bash
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 \
  --before default_reset --after soft_reset \
  write_flash <绝对路径>/nuttx.bin@0x12010000
```

上电后手表直接进表盘（串口 1M 波特可看启动日志）。无手机也可完整使用：表盘/菜单/工具/运动跌倒检测；连接手机 App 后启用时间同步、提醒、AI 闭环与事件上报。

### 4. 跌倒检测演示

佩戴或手持手表，从 ~30cm 松手使其自然跌落到软质平面并保持静止：约 5~8 秒后弹出**红色全屏倒计时**，按 KEY2 或点 CANCEL 取消；不取消则倒计时结束上报"疑似跌倒"事件。Exercise 页的 Record 按钮可录制原始六轴数据（串口 CSV），用 `tools/fall_threshold_analysis.py` 可离线复现检测判定。

## 五、AI Coding 使用说明

本作品全程与 AI 结对开发（Claude/ZCode），覆盖：

- **方案设计**：里程碑规划（M0~M7）、BLE 协议帧格式设计、跌倒算法的离线定标流程设计
- **编码实现**：应用框架、BLE 桥接/协议层、运动服务与跌倒状态机、LVGL 页面
- **调试定位**：从串口日志定位 BLE CCC 订阅不匹配、`bt_gatt_indicate` 同步阻塞导致触控冻结、跌倒角度基准初始化错误等根因（见 `docs/development/` 各开发日志的"根因记录"）
- **测试分析**：6 组实采数据回灌固件同源算法做阈值定标与误报/漏报验证

完整对话日志见 `logs/`（按官方《AI Coding 日志归集与提交手册》导出）；过程性记录见 `docs/development/` 下按日期的开发日志。

## 六、当前状态（诚实声明）

- 已实机验证：显示/触摸/按键、菜单与应用框架、DHT22、计步/活动分类、跌倒检测本地全流程（告警/取消/事件日志）、离线阈值定标（6/6 通过）
- 排查中：BLE 控制器（LCPU legacy patch）在连接建立阶段断言崩溃，已排除应用层因素，详见 `docs/development/BLE连接崩溃排查提示词-2026-09-02.md`；依赖 BLE 的功能（时间同步、AI 闭环、事件上报）在此问题解决后做最终回归
