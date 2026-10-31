# AI Watch —— 基于 openvela 的多功能 AI 腕上助手

> openvela AI 硬件开发者大赛 · 队伍 403（anpai）· 硬件：立创·黄山派（SF32LB52）

## 一、作品简介

**AI Watch** 是一台跑着 openvela（NuttX 内核）的智能腕表：在黄山派开发板的屏幕、触摸、按键之上，实现了一套手表应用平台与手机互联协议，并围绕"运动与安全"做出了核心亮点——**基于板载六轴传感器的疑似跌倒检测**：检测到疑似跌倒后全屏 15 秒倒计时，可按键/触摸取消，超时自动上报手机；检测算法（合加速度冲击、自由落体、姿态翻转、冲击后静止四要素）先经 6 组真实测试集离线定标再上板（详见开发日志 2026-09-02）。

功能一览（截至 v4.8.46）：

- **表盘与应用平台**：翻页表盘、六边形应用菜单（hex menu）、三主题、手势导航 + KEY2
- **运动与跌倒检测**：计步/距离、静止/步行/跑步分类、疑似跌倒检测与告警流程、原始数据采集模式（串口全量 CSV + BLE 抽样流）
- **手机互联（BLE GATT）**：时间同步、提醒/通知下发与到点条幅、传感器数据与事件上报、AI 语音闭环触发
- **AI 助手**：手表一键触发 → 手机 ASR/LLM → 结果回显，可落地为提醒/倒计时
- **显示与设置**：LTR-303 环境光自动亮度（1–100 手动可调）、滚动设置页（亮度条 / 主题条）、电池电量图标与百分比
- **设备端 Skill**：久坐提醒、步数达标祝贺、每日简报三个本地技能，按时间或阈值自主触发，独立线程运行，拨盘演示模式可压缩阈值
- **AI 负一屏**：表盘左滑进入，显示当日步数/活动分钟/跌倒次数、手机同步的天气与一句 AI 点评、尚未到点的提醒；无手机时显示本地统计
- **实用工具**：倒计时、提醒（含到点条幅）
- **桌面远程操控台**：单串口三合一（投屏 + 触控回传 + 固件日志），表盘玻璃意外损坏后构建的全功能替代通道，坏屏可用、插板自动投屏，附 13 + 7 项自动化回归（详见"运行方式"第 5 节）

## 二、选题方向

**AI 硬件产品创新**（手表应用形态，自定方向）。选择黄山派 SF32LB52：屏幕/触摸/按键/蓝牙/板载运动传感器齐备，适合腕上形态；openvela 的 LVGL + NuttX 栈成熟，可以把精力放在"跌倒检测这类真实痛点 + AI 手机协同"的产品逻辑上。

## 三、目录结构

```
app/ai_watch/      # AI Watch 主应用（LVGL UI、应用框架、运动服务、BLE 协议层）
  ├─ ai_watch_main.c      # 应用框架、页面、按键/触摸、跌倒告警 UI、设置页
  ├─ ai_watch_motion.c/h  # 后台运动服务：采样线程、计步/活动/跌倒算法、数据采集
  ├─ ai_watch_ble.c/h     # 协议层：TimeSync/命令解析/提醒存储/上行队列
  ├─ ai_watch_light.c/h   # LTR-303 环境光采样与自动亮度（亮度由主线程落地，避免与 LVGL 抢 LCDC）
  ├─ ai_watch_battery.c/h # GPADC ch7 电池电压采样、滤波与电量估算
  ├─ ai_watch_shot.c/h    # 远程操控台固件侧：帧快照/RLE 编码/非阻塞分片发送、主机触摸注入解析
  ├─ ai_watch_icons.*     # 图标资源（用户图标集生成）
  └─ fonts/, icons/       # CJK 字体与图标源
board/ai_watch/    # 板级 defconfig（映射到 vendor/openvela/boards/contest2026_403_ai_watch）
phone/             # 手机端配套 App 源码（Android/Kotlin/Compose，BLE 互联与 AI 闭环的另一半）
docs/development/  # 开发日志（按日期）、手机端需求文档、排查文档
tools/             # fall_threshold_analysis.py 跌倒阈值定标；ble_session_stats.py BLE 会话统计；
                   # shot_protocol.py / shot_remote.py / shot_selftest.py / watch_device.sh 远程操控台主机侧；ble_adv_lottery.py / ble_connect_test.py BLE 连接回归
logs/              # AI Coding 日志（按官方手册导出提交）
```

> 代码经 manifest `<linkfile>` 映射进 openvela 编译树（`packages/demos/contest2026_403_ai_watch/` 等）。作品对公共仓有两处配套修改，已按规则向 `dev-ai-contest-2026` 分支提交 PR（组委会评审中）：[vendor_sifli#32](https://github.com/open-vela/vendor_sifli/pull/32)（SF32LB52 BLE 桥接 zblue 适配、LCPU patch 加载修复、控制器自愈、LCD 注册竞态、黄山派板级 rcS/ROMFS 调整——实机 BLE 连接与启动依赖此 PR）；[frameworks_runtimes_feature#4](https://github.com/open-vela/frameworks_runtimes_feature/pull/4)（move ctor 编译修复——全量构建依赖此 PR）。

## 四、运行方式

### 1. 拉取工程

```bash
repo init -u https://github.com/open-vela/contest2026_403_anpai \
  -b dev-ai-contest-2026 -m contest2026_403_anpai.xml
repo sync -c -j8
```

### 2. 编译（openvela 工作区根目录）

```bash
cmake -G Ninja -S nuttx -B cmake_out/prod -DNUTTX_BUILD=y \
  -DBOARD_CONFIG="$PWD/vendor/openvela/boards/contest2026_403_ai_watch/configs/ai_watch" \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS"
cmake --build cmake_out/prod -j 12
# 产物：cmake_out/prod/nuttx.bin
```

（已配置过的树只需 `cmake --build cmake_out/prod -j 12`。注意 `-Wno-error=return-mismatch` 在 GCC 13.4 上不存在，不要加。若容器禁用 `utimensat`，需把 touch 包装器目录前置到 PATH 并把 `TMPDIR` 指到大分区——这是开发容器的限制，普通 Linux 不需要。应用经 `CONFIG_EXAMPLES_AI_WATCH=y` 启用，已写入 defconfig。）

### 3. 烧录与运行

```bash
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 \
  --before default_reset --after soft_reset \
  write_flash <绝对路径>/nuttx.bin@0x12010000
```

上电后手表直接进表盘（串口 1M 波特可看启动日志）。无手机也可完整使用：表盘/菜单/工具/运动跌倒检测；连接手机 App 后启用时间同步、提醒、AI 闭环与事件上报——手机端 App 源码与构建方式见 `phone/`。

### 4. 跌倒检测演示

佩戴或手持手表，从 ~30cm 松手使其自然跌落到软质平面并保持静止：约 5~8 秒后弹出**红色全屏倒计时**，按 KEY2 或点 CANCEL 取消；不取消则倒计时结束上报"疑似跌倒"事件。Exercise 页的 Record 按钮可录制原始六轴数据（串口 CSV），用 `tools/fall_threshold_analysis.py` 可离线复现检测判定。

### 5. 桌面远程操控台（投屏 + 触控 + 日志，坏屏可用）

表盘玻璃在开发后期意外摔坏。我们据此在固件内实现了一套**零新增硬件**的替代通道：把既有的 1 Mbaud 调试串口复用为"帧流 + 触摸回传 + 固件日志"三合一，手表进入无屏（headless）渲染模式，在电脑上获得一个可完整操作的表盘——应用菜单、设置、跌倒告警流程均可远程演示，也是无屏状态下唯一的演示与运维通道。

使用：

```bash
python3 tools/shot_remote.py            # 打开 X11 预览窗口，鼠标点击/拖动 = 手表触摸
systemctl --user enable --now ai-watch-viewer.service   # 可选：插上板子自动投屏，拔掉自动关闭
python3 tools/shot_selftest.py --unit   # 解析器合成流 13 项回归（无需板子）
python3 tools/shot_selftest.py --live   # 实机 7 项回归：吞吐/上滑/拖拽/单击/连点/停顿/响应
```

优化前后对比：初版逐帧同步写串口，一帧秒级且点击必卡死；当前版本静态画面 5~9 fps（稳态实测 9.2 fps），点击、上滑（进应用菜单）、菜单拖拽全部即时生效。工程要点：

- **不阻塞主循环的帧发送**：LVGL 每轮主循环只把串口 TX 环填满即让出（NuttX 串口 `O_NONBLOCK` 分片续传），帧期间触摸与动画照常；配套把 `CONFIG_UART_TXBUFSIZE` 256→2048（原值把吞吐卡在 6 KB/s，仅为线速 1/16）
- **带宽适配**：2× 盒滤波降采样 + RLE 压缩（静态表盘一帧 ~3.7 KB），协议带魔数重同步；传输中的调试日志自动挂起、帧完成后补发，避免撕裂帧流
- **主机侧**：`select` 双路（X 事件与串口字节互不阻塞）、增量帧解析、撕帧自动重发请求
- **自动化**：`shot_selftest.py` 退出码可判 CI；root-cause 全程见 `docs/development/开发日志-2026-10-31-坏屏投屏链路优化.md`

## 五、AI Coding 使用说明

本作品全程与 AI 结对开发，使用 mimo（Claude Code）、glm5.3flash（ZCode）、gpt5.6sol（Codex），覆盖：

- **方案设计**：BLE 协议帧格式设计、跌倒算法的离线定标流程设计
- **编码实现**：应用框架、BLE 桥接/协议层、运动服务与跌倒状态机、LVGL 页面
- **调试定位**：从串口日志定位 BLE CCC 订阅不匹配、`bt_gatt_indicate` 同步阻塞导致触控冻结、跌倒角度基准初始化错误等根因；坏屏后远程操控台"点一下卡死"的三层根因链（LVGL 主循环阻塞 I/O → 串口 TX 环 256 字节隐性瓶颈 → 增量构建未再生配置头的静默失效），以及"幻影连点"测试伪象的证伪（见 `docs/development/` 各开发日志的"根因记录"）
- **测试分析**：6 组实采数据回灌固件同源算法做阈值定标与验证

完整对话日志见 `logs/`（按官方《AI Coding 日志归集与提交手册》导出）；过程性记录见 `docs/development/` 下按日期的开发日志。

开发经验已沉淀为可复用的 AI Skill 并随仓提交：`.agents/skills/ai-watch-playbook/`——从 `docs/development/` 下的开发日志提炼的 AI Watch 开发手册（红线规则 + LVGL/BLE/传感器/跌倒算法/调试方法论），用 ZCode / Claude Code 打开本仓库即可自动加载使用。
