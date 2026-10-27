# BLE：zblue 桥接、协议设计、上报队列、自愈与控制器崩溃

## 架构事实（SF32LB52 + openvela）

- 层级：应用 → 桥接（vendor/sifli/chips/sf32lb52/sf32lb52_ble_bridge.c）→
  zblue（external/zblue，Zephyr 移植，API 带 `_mc` 后缀如
  `bt_gatt_service_register_mc`）→ H4 `/dev/ttyHCI0` → LCPU 控制器。
- zblue 头文件不可直接包含（依赖 Zephyr syscall 头）；BSP 留弱钩子
  `sf32lb52_bt_register_custom_services()`，应用侧给强实现注册 GATT。
- 广播名 "AI-Watch-403"；地址 CD:AB:78:56:34:12 (public)；HCI 5.3。

## 自定义 GATT（f1~f4）

```
Service 12345678-9abc-def0-1234-56789abcdef0
f1 Status    Read+Notify   [ver][conn][last_sync u32]
f2 TimeSync  Write         [ver][utc u32][tz_min s16] → clock_settime
f3 DataUpload Notify/Ind    [ver][sensor_type][payload...]
f4 Command   Write         [ver][cmd][id][flags][ts u32][len][title]
```

- 帧首字节永远是 version；全小端；未知 cmd_type 忽略（向前兼容）。
- f3 sensor_type：0x01 TEMP / 0x02 HUM / 0x03 HR / 0x04 SPO2 /
  0x05 STEPS / 0x06 ACTIVITY / 0x10 AI_TRIGGER / 0x11 FALL_EVENT /
  0x12 MOTION_DATA。周期类上报全部频控（≥5s 或事件驱动）。
- 命令帧上限 240B（`AI_WATCH_BLE_CMD_MAX`），放长 title 前先核 MTU≥247。

## CCC 订阅：0x0001 Notify / 0x0002 Indicate 都要兼容

手机端可能写任一种 CCC。桥接记录各特征的 CCC 值，发送时按值分派：
0x0002 → `bt_gatt_indicate`（静态 params + pending 标志去重，pending 时
-EAGAIN 丢弃），否则 `bt_gatt_notify`；断连清 CCC 缓存与 pending。
特征声明要同时带 NOTIFY|INDICATE，否则一方报
"Device is not subscribed"（host 层 -EINVAL，**无空口流量**）。

## 指示/通知的同步阻塞 → TX 队列线程

- **`bt_gatt_indicate()` 同步阻塞约一个连接间隔（实测 ~120ms/次）**。
  主循环里按周期发 = 主循环被吃掉（4.8.0 触摸冻结的根因）。
- 模式：`ai_watch_ble_post(type, data, len)` 入队（48 深，单生产者=
  主循环），专用低优 TX 线程消费；发送失败退避 2s（否则 host 层告警
  会刷屏，曾 119 条）；队列满/未连接直接丢弃并计数——丢数据好过堵 UI。
- 只有需要同步反馈的单发（AI 触发按钮）走直接发送。
- 新连接后的首次上报：CCC 可能还没写，失败退避即可，**绝不能循环重试**。

## 两个已知"必踩"陷阱

1. **HCPU SRAM 顶部 1KB（0x2007FC00-0x2007FFFF）是 HCPU↔LCPU 邮箱窗口**，
   CH1 通道承载 BT HCI 流。`up_allocate_heap()` 原版把堆终点设到 SRAM_END
   ——邮箱窗口落在堆里，堆布局一变（如固件瘦身）就覆盖 HCI 流，
   LCPU 报 `Hardware error, hardware code: 0`、服务发现全超时。
   修复：堆终点回退到 0x2007FB00（`HCPU_TOP_RESERVED=0x500`，
   见 `vendor/sifli/chips/sf32lb52/sifli_allocateheap.c`）。启动日志的
   heap arena 是总堆（含 PSRAM），SRAM 余量还要看 map/.bss。
2. **LCPU 补丁分代**：revid < A4 → legacy patch，A4+ → rev_b
   （`sf32lb52_lcpu_boot.c`）。补丁 blob 必须同步上游 HEAD（历史上
   rev_b 缺 "connection update dump" 与 "ld stop assert" 两个修复导致
   HW Error）。启动日志 `lcpu patch: revid=N -> xxx patch` + 
   `lcpu evidence: ...`（revid/补丁路径/assert 记录/配置字）是向 SiFli
   索证的证据，升级诊断时打印的是这几行。

## 自愈链（已在 4.5.0 实测通过一次完整循环）

`Hardware error` → 桥接 recovering → HCI_RESET → LCPU 断电重启 →
重新广播 → GATT 重暴露 → 手机重连。恢复期两行
`L2CAP/ATT BR server registration failed -98`（EADDRINUSE，旧代残留注册）
与 `advertising start failed -11/-114` 是**瞬时噪音**，LE 侧不受影响，
留档观察即可——不要把它们当新故障去修。

## 排查纪律

- 先分清 host 层错误（zblue 打印，如 not subscribed / -EINVAL）与
  控制器层错误（`Hardware error` + `lcpu evidence`）——前者查我们的
  发送逻辑，后者查邮箱窗口/补丁代际/硅片 errata。
- 手机侧规避手段（会诊结论）：操作串行化、连接后延迟再发、失败退避、
  勿请求连接参数更新。
- 历史未决（已止损）：4.8.3 起连接建立阶段 LCPU 断言（任意 central 必现）。
  2026-10 终态 = E1 新版 legacy 补丁 + legacy 广播 + HCI 3s 自愈，见末节
  「稳定性修复的硬依赖与改动边界」；过程证据在
  `docs/development/2026-10-26-ble-r7-r8-hook-disproof-legacy-and-fast-recovery.md`。

## 稳定性修复的硬依赖与改动边界（2026-10-26 交接）

当前 BLE 稳定性的终态 = E1 LCPU blob + legacy 广播 + HCI 3s 快速自愈 +
邮箱窗口/栈布局保护。它只对下面四类改动敏感；普通 UI/算法开发绕得开。

1. **内存布局**
   - `0x2007FC00-0x2007FFFF` 是 HCPU↔LCPU 邮箱，CH1 承载 HCI 流；堆终点由
     `HCPU_TOP_RESERVED=0x500` 钉在 `0x2007FB00`（`sifli_allocateheap.c`）。
   - 不要改该常量/链接脚本/内存映射；不要对邮箱窗口做 dcache invalidate（红线 #13）。
   - 新增静态 RAM 会压缩 SRAM 堆：每阶段结束看 `.bss` 与 SRAM 堆余量，
     给 zblue 的连接/缓冲分配留够空间。
2. **LCPU 补丁 blob（E1）**
   - `vendor/sifli/middleware/bluetooth/patch/sf32lb52/sf32lb52_lcpu_patch_exp.h`
     （8160B，8KiB patch 区只剩 ~32B），由 `sf32lb52_lcpu_boot.c` 的 include 指向。
   - **目前是工作树未提交修改**：`git checkout` / `repo sync` / 换机器就会丢，
     连接期 Hardware Error 复发。动 vendor/sifli 前先 `git status` 确认三份文件还在；
     最终要提 vendor_sifli PR 或归档到参赛仓 `patches/`。
3. **zblue 自愈**
   - `hci_core.c` 的 `HCI_CMD_TIMEOUT 3s`、`hdev->sync_sem`（不要改回栈上信号量）、
     timeout/hw_error/cc_status 三个回调。代码在内层仓本地 `contest/ai-watch` 分支，
     不要 sync/reset 回上游。
4. **Kconfig / 栈**
   - `BT_EXT_ADV=n`、`BT_SCAN_AND_INITIATE_IN_PARALLEL=n`（legacy 广播是稳定前提）。
   - `ALLSYMS/ELF=y`（T1 实证：关掉会 BLE init 风暴）。
   - `PTHREAD_MUTEX_TYPES=y` + `LIBCXX=n`（不钉回 h4.c 编译失败）。
   - INIT/DEFAULT_TASK_STACKSIZE=64KB、IDLE/workqueue=32KB；不要缩。

### 后续规划的影响速查

- **不碰控制器/自愈层**：P1-2、P1-4、P2（模型/活动识别）、P4、P5-1~P5-6
  （P4/P5-3/P5-4 若复用提醒或事件通道，仍属现有 Command，低速率）。
- **只碰应用协议层**：P3-1 日报（新增 Command 0x06）、P3-2/P3-3、P5-8 媒体/找手机。
  复用现有 f4 Command 特征，**不新增/重排 GATT attribute**（避免 Android handle
  缓存）；周期数据仍 ≤4 帧/s；P5-8 不要启用 AVRCP/Classic。
- **间接影响时序/内存**：P1-3/P2-3 flash 持久化（后台分块写，避开连接/校时窗口；
  flash 写擦会停 XIP/占 CPU）、P5-7 新 I2C 传感器（i2c1 与 IMU 共享总线，
  后台线程 + 短事务）。
- **P1-1 唯一接触点**：`BLE_REC_DEC` 随采样率同步加大，保持 ~4 帧/s。

### 电池 / 电量显示（2026-10 下旬新增）

- 结论：**不改 BLE 代码，风险低。**
- 采集通路已就绪：板子已有 AW32001 充电 IC，加电池不需要新驱动；`/dev/adc0`
  初始化为内部 VBAT 通道（`ADC_CHAN_5`，已校准、20 次平均），不需要外接分压
  或新 I2C 设备，1Hz 读取即可。充电时读到的可能是 VSYS/USB 侧电压而非电池
  电压，需实测确认并在 UI 上区分/标注。
- 充电 IC AW32001 在硬件 I2C2（NuttX i2c1）0x49，目前只在
  `BSP_PowerUpCustom(is_deep_sleep=true)` 里用临时 HAL 句柄配置，不注册 NuttX
  驱动；正常运行时无总线争用。以后若要读充电状态，按 i2c1 低速率设备处理。
- 真正要实测的是电气/RF，不是软件：
  1. 低电量下 BLE 连接/校时/提醒是否稳（TX 电流尖峰 + 电池内阻 → 压降/复位）；
  2. 边充电边连接时的 RF/ADC 噪声；
  3. 电池摆放是否靠近天线（可能失谐，影响距离/RSSI）；
  4. 低电压时不要启动 flash 写擦（与 P1-3 联动，防止掉电写坏）。
