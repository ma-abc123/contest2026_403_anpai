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
   修复：堆终点回退 1KB（SRAM_END -= 0x400）。启动日志的 heap arena
   行就是这项检查的哨兵。
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
- 当前未决：4.8.3 起连接建立阶段 LCPU 断言（任意 central 必现），
  证据与排除清单见 `docs/development/BLE连接崩溃排查提示词-2026-09-02.md`。
