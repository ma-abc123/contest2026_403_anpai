# AI Watch —— 基于 openvela 的腕上运动安全助手

> openvela AI 硬件开发者大赛 · 队伍 403（anpai）· 硬件：立创·黄山派（SF32LB52）
> 当前代码版本 **4.8.81**（`test` / `panel` / `capture` 三种构建，由预处理器开关区分，代码只有一份）；
> 真机验收：**4.8.81-vendor-panel**（2026-11-04：新屏触控与逐帧刷新已验；BLE 连接与校时 5 轮 4 次通过）

## 一、作品简介

AI Watch 是一台跑 openvela（NuttX）的智能腕表。硬件侧有 390×450 屏幕、触摸、KEY2、BLE 与板载六轴 IMU；软件侧实现了一套手表界面框架、一个常驻运动服务、一套手机互联协议，并围绕**运动与安全**做成完整闭环：

**检测疑似跌倒 → 全屏 15 秒倒计时 → 可取消 → 超时上报手机**。

跌倒判定是**确定性规则算法**（冲击峰值、自由落体、姿态翻转、冲击后静止四要素），不是神经网络：它可在离线用同一套状态机复现、每个阈值都能单独扫描，便于复盘和调参。手机端 App 负责时间同步、提醒下发与 AI 闭环，手表侧只做触发与呈现。

开发后期手表屏幕意外摔坏，我们没有停摆，而是在固件里做了一套**零新增硬件**的替代通道：把调试串口复用成"投屏 + 触摸回传 + 日志"三合一，此后所有 UI 开发与演示都在电脑上完成——这段经历反而成了本作品最有辨识度的工程部分之一。

## 二、系统构成

```
┌──────────────── 手表（openvela / NuttX，SF32LB52）────────────────┐
│  表盘  │  负一屏  │  应用菜单（AI/Timer/Reminder/Settings/Exercise）│
│  跌倒告警模态层（15 s 倒计时 + CANCEL）                            │
├──────────────────────────────────────────────────────────────────┤
│  运动服务线程        │  Skill 线程        │  BLE 协议层            │
│  25 Hz 采样 → 计步/   │  久坐 50 min       │  Status / TimeSync     │
│  活动分类 / 跌倒状态机│  步数目标 6000     │  Command / DataUpload  │
│                      │  每日简报定时槽     │  （0x05/0x06/0x10~0x12）│
├──────────────────────────────────────────────────────────────────┤
│  LVGL 9  │  远程操控台（帧流 + 触摸注入 + 日志复用单串口）          │
└──────────────────────────────────────────────────────────────────┘
                     ↕ BLE GATT
┌──────────────── 手机 App（Android / Kotlin / Compose）────────────┐
│  连接与校时、提醒下发、跌倒事件入库与告警通知、AI 语音闭环          │
└──────────────────────────────────────────────────────────────────┘
```

## 三、功能全景

### 1. 界面与交互

| 功能 | 说明 |
|---|---|
| 翻页表盘 | 时间/日期、电量图标与百分比、BLE 状态、上滑提示；左右滑动是"相邻页"语义 |
| 负一屏（表盘右滑进入） | 当日步数 / 活动分钟 / 跌倒次数、天气卡（28 px 字体，含 ℃ 与 6 种天气字：晴/多云/阴/雨/雪/雾）、一句 AI 点评、待触发提醒；页面可上下拖拽滚动 |
| 应用菜单 | 六边形蜂窝布局，拖动、吸附居中、按距离缩放；5 个应用 |
| 页面框架 | 页面栈（深度 8）、按键/手势统一分发、200 ms 淡入；坏屏模式下改为瞬时切换 |
| 主题 | 深色 / 浅色 / 蓝色三套，设置页可切换并即时生效 |
| 设置页 | 亮度条（1–100%，拖动时锁定手势避免误返回）、环境光自动亮度开关、主题条、蓝牙开关 |

### 2. 运动与跌倒

| 功能 | 说明 |
|---|---|
| 采样 | lsm6dsl 单次 burst 读（0x22–0x2D 一帧取回），应用层实测 **25 Hz**（本轮 REC 日志：间隔中位 40 ms，p10=p90=40 ms） |
| 计步 | 峰值/谷值双阈值 + 300 ms 不应期，含距离估算 |
| 活动分类 | REST / WALK / RUN 三态，供负一屏与 Skill 使用 |
| 跌倒检测 | 四要素规则状态机，见第五节 |
| 告警流程 | `lv_layer_top` 全屏红色模态层，15 s 倒计时，触摸 CANCEL 或 KEY2 取消；超时上报；取消也上报（事件码区分） |
| 测试触发 | Exercise 页 `Test` 按钮，走**同一条状态机与上报链路**，上报事件码为 3（测试），不计入当日跌倒次数 |
| 数据录制 | Exercise 页 `Record`，串口输出全量六轴 CSV（`REC,<t_ms>,ax,ay,az,gx,gy,gz`），供离线复现 |

### 3. 设备端 Skill（独立线程，时间/阈值自主触发）

| Skill | 触发条件 | 行为 |
|---|---|---|
| 久坐提醒 | 连续 50 min 活动分类为 REST | 弹条幅提醒起身 |
| 步数目标 | 当日步数达到 6000 | 弹祝贺条幅 |
| 每日简报槽 | 到达设定的时间点 | 配合手机同步内容刷新负一屏 |

拨盘演示模式会把阈值压缩（久坐 20 s、步数目标 20 步），方便现场演示。

### 4. 手机互联（BLE GATT）

| 特征 | UUID 尾号 | 方向 | 载荷 |
|---|---|---|---|
| Status | `…def1` | 手表 → 手机（read/notify） | `version(1) + conn_state(1) + last_sync(4)` |
| TimeSync | `…def2` | 手机 → 手表（write） | `version(1) + utc(4, LE) + tz_offset_min(2, LE)` |
| DataUpload | `…def3` | 手表 → 手机（notify） | `version(1) + sensor_type(1) + payload` |
| Command | `…def4` | 手机 → 手表（write） | 提醒 / 通知 / 清空 / AI 文本 / AI 倒计时 |

DataUpload 的 `sensor_type`：`0x05` 步数、`0x06` 活动、`0x10` AI 触发、`0x11` 跌倒事件、`0x12` 原始运动流。
跌倒事件载荷：`event(1) + impact_mg(2, LE) + angle_deg(1) + reserved(4)`，事件码 1=确认、2=已取消、3=测试。

### 5. 远程操控台（坏屏后的替代通道）

| 能力 | 实现 |
|---|---|
| 投屏 | LVGL 帧经 2× 盒滤波降采样 + RLE 编码，静态表盘约 3.7 KB/帧，魔数重同步，撕帧自动重发 |
| 触摸回传 | 主机发送 `T + x(2) + y(2) + state(1)`，固件解析为 LVGL pointer indev，点击/拖动与真触摸等价 |
| 日志复用 | 同一串口输出固件日志；帧传输期间日志挂起、帧结束后补发，避免撕裂帧流 |
| 不阻塞主循环 | 每轮只把 TX 环填满就让出（`O_NONBLOCK` 分片续传），`CONFIG_UART_TXBUFSIZE` 256→2048 |
| 自动化 | `tools/shot_selftest.py --unit` 13 项解析器回归（免板子）、`--live` 7 项实机回归；`watch_device.sh` 插板自动投屏 |

静态画面实测 **5–9 fps**（启动后紧测 5.2 fps，稳态 9.2 fps），点击、上滑、菜单拖拽均即时生效。

### 6. 工具体系

| 工具 | 用途 |
|---|---|
| `tools/fall_threshold_analysis.py` | 串口日志按会话切分，用**固件同源状态机**回灌，输出统计与 PASS/FAIL，阈值全部可扫描 |
| `tools/build_capture.sh` | 一键编采集固件（上电即连续输出 `REC`/`STEP`，蓝牙关闭） |
| `tools/capture_display.py` + `run_capture.sh` | 桌面采集提示窗：显示当前动作、剩余秒数、实时检出与步数，采集完自动出摘要 |
| `tools/capture_analyze.py` | 事后分析：质量报告（采样率/断流）+ 按动作切分 + 每段失重时长/峰值/角度/步数 + TP/FP/FN/TN |
| `tools/ble_adv_lottery.py` / `ble_connect_test.py` | 逐 boot 广播成功率统计、连接→订阅→校时全流程回归 |
| `tools/build_panel.sh` | 一键编正屏固件（`AI_WATCH_HEADLESS=0`） |

## 四、决赛阶段做的优化

这一段是 10-24 之后针对"能不能流畅跑、能不能讲清楚"做的专项，尽量给出前后数字与出处；没有出处的只写趋势。

### 1. 固件瘦身：5.14 MB → 2.54 MB（此后功能增加，当前 2.63 MB）

按 G1~G6 分组逐组关掉无用配置（LVGL demo/字体/驱动、NuttX 未用子系统、调试符号等），每组单独回归。P0 收尾时 2.54 MB；此后加入天气卡、设备端 Skill、采集构建等，当前产物 2,632,668 B（约 2.63 MB，16 MB 的 16%）。SRAM 静态约 320 KB。过程中排掉两个会静默失效的坑：**改 defconfig 内容不会触发重新配置**、**apps 源码目录里的旧生成头文件会遮蔽新配置**。

### 2. 采样提速：15 Hz → 25 Hz（配置 833 Hz ODR 的 burst 读）

原读路径每样本走 22 次单字节寄存器读（约 42 ms），有效采样只有 ~15 Hz，冲击峰值有欠采样风险。改成 burst 一帧读回后压到 1 ms，采样节拍由休眠精度决定。**当前实测 25 Hz**（40 ms 中位间隔，抖动极小），25 Hz 对 120–300 ms 的冲击仍有约 3–7 个样本。

### 3. 新屏到货后的两个应用层问题：触控被顶掉、渲染优化不成立

新屏装上后表现为"屏能亮、触控全无、画面一帧一帧地刷"。两个症状都出在应用层，与驱动和硬件无关。

**触控**：`ai_watch_shot` 无条件创建的主机鼠标 indev，在面板触摸尚未注册时被当成触摸设备绑定，并把轮询计数清零，轮询就此终止。触摸控制器由 `sifli_ap.c` 的 `lcd_async_init_thread` 异步带起（80 ms settle 后才调 `ft6146_touch_initialize`），启动瞬间 `/dev/input0` 常常还不存在，等它真出现时已经没人绑定了。修法是加一个 `touch_panel_ready` 区分"面板控制器"与"临时主机指针"：面板模式下兜底只提供临时输入、不清零轮询，主循环一旦找到 `/dev/input0` 就删掉临时 indev 并切到面板；headless 行为不变。

**渲染**：
- 根因不是刷新率，而是**整屏 351 KB 缓冲在 PSRAM 里软渲染**（实测带宽约 4 MB/s），加上脏区计算失效
- 2026-11-01 改成 **PARTIAL 条带渲染**：静态 `.bss` 条带（390×60 RGB565，46.8 KB，`aligned(64)` 落 SRAM）+ `LV_DISPLAY_RENDER_MODE_PARTIAL`，配一块 351 KB 影子帧；headless 下负一屏滚动单帧 88 ms → 55–90 ms
- **但这组数字只在 headless（完全不写面板）下成立。** 换到真屏后，PARTIAL 每帧产生几十次 `PUTAREA`，每次都要重设面板窗口，玻璃上表现为一条一条地刷
- 又试了 `RENDER_MODE_DIRECT` + 自建第二个 `/dev/lcd0` fd 接管 flush：画面能出，但只有触摸时才推进一帧——两个 fd 写同一设备、外加外来 `flush_cb` 打乱了刷新握手
- **最终回退到端口原生路径**：真屏用 FULL 渲染 + 驱动自带 `flush_cb` + 不额外开绘制 fd，影子帧降级为只读镜像供投屏；PARTIAL 条带只保留在 headless 构建
- 留下的是一个反例：**benchmark 的测量环境决定了结论的适用范围**——在没有面板的机器上测渲染，测不到面板写入的代价

真机验证（4.8.81-vendor-panel，2026-11-04）：触控跟手、时钟走秒与蓝牙状态正常刷新、滑动不再条带化。

### 4. 投屏链路：从"点一下卡死"到 9.2 fps

初版逐帧同步写串口，一帧要秒级、点击必卡死。三层根因依次是：LVGL 主循环被阻塞 I/O 拖住 → 串口 TX 环 256 字节把吞吐卡在 6 KB/s → 增量构建没重编配置头导致"改了没生效"。修完加上降采样 + RLE 后，静态 9.2 fps，交互即时。

### 5. 负一屏天气卡：一个"加两个对象就废掉蓝牙"的红线

天气卡最初用"容器 + 两个 label"实现，结果**每次开机 `bt_enable -110`**。二分了 6 个变量（字体、字符串、堆余量、堆顶保留区……）后确认唯一相关变量就是**多出来的 2 个 LVGL 对象**，机制指向对象分配改变堆相位后触发 vendor 侧写越界落进 BT 共享内存窗口。修复是绕行：**把卡片改成单个样式化 label**（背景/边框/圆角/内边距都设在 label 上），对象数与之前持平，蓝牙立即恢复。

这条成了项目红线：**改任何页面先数对象数，改完必跑 BLE 广播回归**。

### 6. 跌倒误报修复：姿态路径必须带自由落体

现场使用中"拿起手表转一下再放下"会误报。复盘发现原判定允许**零失重冲击 + 姿态变化**直接报警，而这条路径正是"拿起来翻转放下"的形状。三处修改：

| 项 | 原值 | 现值 | 理由 |
|---|---|---|---|
| 姿态路径前置条件 | 无 | **必须检测到自由落体** | 关掉零失重冲击的误报入口 |
| 有失重时的静止门限 | 80 mg | **300 mg** | 床垫落地后余震 315–606 mg，原门限把真跌倒判成"还在动" |
| 姿态角门限 | 50° | **40°** | 软垫上跌落常只转到 45° 左右 |

改后坐姿模拟摔倒 2 次均触发（5.6 s / 7.6 s 出告警）。

### 7. 采集与定标工具链

因为要判断"该报的有没有报、不该报的有没有报"，做了一整套：采集固件（上电即采，零按键交互）→ 桌面提示窗（大字显示第几个动作、剩余秒数、实时检出）→ 事后分析（按动作切分，输出每段的失重时长、冲击峰值、终态重力方向、步数，并给出 TP/FP/FN/TN）。采集构建还顺手修掉两个会让数据作废的问题：**BLE 控制器每 10 s 恢复一次会打断采样**（改为采集固件不启动蓝牙，采样率从 22.3 Hz 恢复到 25.0 Hz、零断流）、**60 s 冷却期会吞掉连续跌倒**（采集构建压到 3 s，判定阈值一个没动）。

### 8. BLE 稳定性：把"能广播连不上"拆成可复现的阶段

按故障阶段（广播 / 连接建立 / 链路 / 重连）分别取证，落成 `.agents/skills/ai-watch-ble-diagnostics/`。应用层做了两件事：**INIT_FAILED 退避温和前倾**（首试 +10 s，后续 15/20/30/60/120/300 s，无次数上限——电池设备不能在现场断电重启），**连接窗口隔离**（`CONNECTED` 且尚未校时的 8 s 内暂停帧流、采样降到 15 Hz，窗口结束立即恢复）。

## 五、跌倒判定怎么做的

状态机（每个样本推进一次）：

```
NORMAL ──(冲击 > 2400mg 或 软冲击 2000mg×2 或 自由落体后 2000mg 单点)──> IMPACT
IMPACT ──(3 s 静止窗口；未决则 1 s 步进续看，预算 10 s)──> 判姿态角变化
          ├─ 有自由落体 且 角度 ≥ 40°  → ALARM
          ├─ 有自由落体 且 静止 ≥ 4.5 s（平摔路径）→ ALARM
          └─ 否则 → 回到 NORMAL（并进入较短的不应期）
ALARM  ──(15 s 倒计时结束)──> 上报 FALL_EVENT(event=1) → NORMAL（60 s 冷却）
       └─(CANCEL / KEY2)────> 上报 FALL_EVENT(event=2) → NORMAL
```

几个刻意的设计：
- **只报"疑似跌倒"**：语义与上报文案统一，不做医疗级断言（UI 当前为英文 `Suspected fall`，未做中文化）
- **取消也上报**：手机端能看到"曾经触发又被人取消"，这比"什么都没发生"更有信息量
- **测试事件独立事件码**：按钮触发走同一条链路，但上报 `event=3`，不会污染真实跌倒记录
- 判定参数全部集中在 `ai_watch_motion.c` 顶部，离线工具用同名参数复现

## 六、目录结构

```
app/ai_watch/          # 主应用
  ai_watch_main.c          UI 框架、页面、按键/手势、跌倒告警模态层、设置页
  ai_watch_motion.c/h      采样线程、计步/活动/跌倒状态机、采集构建开关
  ai_watch_ble.c/h         BLE 协议层：TimeSync、命令解析、提醒存储、上行队列、恢复阶梯
  ai_watch_skill.c/h       设备端 Skill（久坐/步数/简报）
  ai_watch_light.c/h       LTR-303 环境光与自动亮度
  ai_watch_battery.c/h     GPADC 电池电压与电量估算
  ai_watch_shot.c/h        远程操控台固件侧：帧快照/RLE/分片发送/触摸注入
  fonts/ icons/            微子集字体（含 28 px 天气字体）与图标
board/ai_watch/        # 板级 defconfig
phone/                 # Android App（Kotlin/Compose）
tools/                 # 采集、投屏、BLE 回归、阈值分析、构建脚本
docs/development/      # 45 篇文档（28 篇开发日志 + 需求/排查/操作卡）
logs/                  # AI Coding 对话日志
.agents/skills/        # ai-watch-playbook（开发手册）、ai-watch-ble-diagnostics（BLE 排查流程）
```

代码经 manifest `<linkfile>` 映射进 openvela 编译树。对公共仓的配套修改已提 PR：
[vendor_sifli#32](https://github.com/open-vela/vendor_sifli/pull/32)（SF32LB52 BLE 桥接 zblue、LCPU patch 加载、控制器自愈、LCD 注册竞态、板级 rcS/ROMFS）、
[frameworks_runtimes_feature#4](https://github.com/open-vela/frameworks_runtimes_feature/pull/4)（move ctor 编译修复）、
[external#52](https://github.com/open-vela/external/pull/52)（zblue 默认构建路径补 HCI H4 TTY 驱动——板级 `CONFIG_BT_H4=y`，缺它链接不过）。

三个 PR 未合入时，在对应公共仓目录下按 `patches/<仓名>/` 的文件名顺序 `git am` 即可；`patches/` 只收录构建必需的最小集，不是完整改动。

## 七、编译、烧录与运行

```bash
# 1. 拉取
repo init -u https://github.com/open-vela/contest2026_403_anpai \
  -b dev-ai-contest-2026 -m contest2026_403_anpai.xml
repo sync -c -j8

# 2. 编译（工作区根目录）
cmake -G Ninja -S nuttx -B cmake_out/prod -DNUTTX_BUILD=y \
  -DBOARD_CONFIG="$PWD/vendor/openvela/boards/contest2026_403_ai_watch/configs/ai_watch" \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS"
cmake --build cmake_out/prod -j 12        # 产物 cmake_out/prod/nuttx.bin

# 3. 烧录
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 \
  --before default_reset --after soft_reset \
  write_flash <绝对路径>/nuttx.bin@0x12010000
```

### 三种构建

| 命令 | 版本号 | 用途 |
|---|---|---|
| `cmake --build cmake_out/prod` | `4.8.81-vendor-test` | 默认：无屏/坏屏模式（headless + 串口投屏，PARTIAL 条带渲染） |
| `tools/build_panel.sh` | `4.8.81-vendor-panel` | 接真屏：FULL 渲染 + 驱动自带 `flush_cb`、启用 FT6146 触摸、页面 200 ms 淡入 |
| `tools/build_capture.sh` | `4.8.81-capture` | 数据采集：上电即连续输出 `REC`/`STEP`，蓝牙关闭 |

> 两个脚本编译完都会恢复宏并**删掉对应 `.o`**。这一步不能省：本开发容器禁用 `utimensat`，`touch` 改不动 mtime，增量构建会沿用上次编译的目标文件——我们踩过：把采集宏带进正常固件，现象是"蓝牙全废、日志刷 REC"。

### 运行

无手机也可完整使用：表盘、负一屏、菜单、运动、跌倒检测、设置。连手机 App 后启用校时、提醒、AI 闭环与事件上报（App 源码见 `phone/`）。

投屏与远程操作：

```bash
python3 tools/shot_remote.py                              # X11 预览窗，鼠标即触摸
systemctl --user enable --now ai-watch-viewer.service     # 插板自动投屏
python3 tools/shot_selftest.py --unit                     # 解析器 13 项回归（免板子）
python3 tools/shot_selftest.py --live                     # 实机 7 项回归
```

### 演示流程

1. 表盘 → 右滑看负一屏（步数 / 活动 / 跌倒次数 / 天气 / 待触发提醒）
2. 上滑进蜂窝菜单 → 打开 Exercise 看步数、活动、实时 |a| 与状态
3. 点 `Test` → 15 s 全屏红色倒计时 → 点 CANCEL 演示"可取消"，或等它走完演示"自动上报"
4. 手机 App 侧查看事件入库与通知
5. 条件允许时做一次真实跌落（从约 30 cm 松手到软垫并保持静止），约 5–8 s 出告警

## 八、数据与状态说明

- **采样率**：应用层实测 25 Hz；`833 Hz` 是传感器 ODR 配置值，不是应用层拿到数据的频率。
- **板级 DHT22 驱动仍在注册**，但 4.8.5 起已移除对应 UI 与上报，本项目不宣称温湿度功能。
- **统计口径**：步数/活动/跌倒次数为**本次运行内**的统计，重启后清零（本板无法在运行期写同一片 NOR，见开发日志 2026-11-03）。
- **跌倒判定依据**：用 6 组自采数据（2026-09-02，采样约 15 Hz，当时阈值 50°/80 mg）以固件同源状态机离线回灌，6/6 通过、对照动作无误报。现行阈值（40°/300 mg）只用 2–3 次模拟动作验证过；样本量有限，**不作为误报率/准确率的统计结论**。
- **UI 实测环境**：屏幕在开发中期损坏，投屏帧率与 headless 渲染耗时均在无屏模式下测得。新屏到货后真屏路径已恢复（4.8.81-vendor-panel，2026-11-04 验证：触控跟手、时钟走秒、蓝牙状态正常刷新），但**投屏 fps 与滚动渲染耗时仍是 headless 数字，真屏未复测**，不当作真屏性能结论。
- **BLE（4.8.81 实测通过连接与校时）**：4.8.10 上曾完成连接 → 订阅 → 校时 → 提醒的全链路（`timesync accepted` / `BLE: time synced`）。4.8.70 时代出现"连接后 0.45–5 s 触发控制器 `Hardware error, code 0`"的必现掉线，根因指向 HCPU 堆顶与 LCPU 邮箱窗口／共享环的缓存一致性。两项改动合入后——HCI 双核共享环映射为非缓存内存、LCPU 补丁恢复厂商原版数组——用 `tools/ble_connect_test.py` 逐轮回归，**5 轮 4 次 SURVIVED**（连接 1.63–1.75 s、校时 2.2–2.3 s、保持 45 s 不掉线，Status notify 正常）；第 5 轮是主机侧扫描没找到设备（`FAIL_CONN`），不是板端掉线。口径：这是主机脚本的自动回归，**不等于手机 App 长稳测试**，现场仍以实测为准。

## 九、AI Coding 说明

全程与 AI 结对开发（mimo/Claude Code、glm5.3flash/ZCode、gpt5.6sol/Codex），覆盖：

- **方案设计**：BLE 帧格式、跌倒定标流程、投屏协议、采集动作表
- **编码实现**：应用框架、BLE 桥接与协议层、跌倒状态机、LVGL 页面与微子集字体
- **调试定位**（日志里都留了根因链）：CCC 订阅不匹配、`bt_gatt_indicate` 同步阻塞导致触控冻结、跌倒角度基准初始化错误、投屏"点一下卡死"的三层根因、增量构建静默失效、LVGL 对象数导致 BLE 致盲
- **测试分析**：采集工具链、离线回灌定标、BLE 逐 boot 广播统计

完整对话日志见 `logs/`，过程记录见 `docs/development/`（28 篇开发日志）。经验沉淀为两个随仓提交的 Skill：`.agents/skills/ai-watch-playbook/`（红线规则 + LVGL/BLE/传感器/调试方法论）与 `.agents/skills/ai-watch-ble-diagnostics/`（BLE 分阶段排查流程），用 ZCode / Claude Code 打开仓库即自动加载。
