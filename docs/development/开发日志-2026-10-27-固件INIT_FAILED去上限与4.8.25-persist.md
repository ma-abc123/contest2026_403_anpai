# 开发日志 2026-10-27（固件：INIT_FAILED 自动恢复去掉次数上限 → 4.8.25-persist）

> 基线：4.8.23-p02 ｜ 改动面：**仅 `app/ai_watch/`**（`ai_watch_ble.c`、版本号）
> 状态：**已构建，未上板验证**（板子当前不在线，无 `/dev/ttyUSB*`）

## 1. 问题

`ai_watch_ble_process()` 里的 INIT_FAILED 自动恢复有 **5 次上限**：`s_initfail_tries < 5`，
间隔 10s 后每 30s 一次，跑满约 2 分钟后**永久停止重试**。此时 BLE 状态停在 `BLE: ERR`，
不再广播，手机永远搜不到——电池供电、现场无法拔电的场景下等于**永久失联**。

这条与 P1-1 §2.4 记录的现象同源：`INIT_FAILED` 跑满 5 次后不再广播，手机侧表现为
「彻底连不上，只能重启/断电」。4.9.9-p11j 曾实现「无限耐心重试」，但整体回退 4.8.23-p02
时一起丢了，本次单独补回。

## 2. 改动（只动应用层计数，不碰桥接/zblue/控制器）

| 位置 | 改前 | 改后 |
|---|---|---|
| `ai_watch_ble.c` 恢复条件 | `state == INIT_FAILED && s_initfail_tries < 5` | `state == INIT_FAILED`（无上限） |
| 间隔 | 固定 30s | 30 / 30 / 60 / 120 / 300s，之后恒定 300s |
| 日志 | `auto-recover attempt %d` | 增加 `(next in %lu s)`，可直接看出阶梯 |
| 计数 | 无钳位 | 钳在 1,000,000（约 5.7 年 @5min，防溢出，纯形式） |
| 版本号 | 4.8.23-p02 | **4.8.25-persist** |

守卫未变：进入 `ADVERTISING / CONNECTED / DISCONNECTED` 任一状态即清零阶梯；设置页
手动开关蓝牙同样清零（视为用户在场、给一次快速恢复机会）。

**为什么用「无上限 + 退避封顶」而不是「无上限 + 固定 30s」**：每次 `recover()` 都是
一次完整 HCI teardown + LCPU 重启，会阻塞主循环（最坏一个命令超时 3s）。控制器真的
坏掉时，30s 一次等于长期占着主循环，反而放大 4.9 线见过的 `-105` 空转风险。5 分钟一次
的代价可以忽略，而恢复窗口仍然存在（最长 5 分钟）。

## 3. 构建结果（实机未验）

```
cmake --build cmake_out/prod -j 12
flash: 2,540,524 B（p02 为 2,540,460 B，+64 B）
sram :   311,316 B（与 p02 完全一致 — 未新增静态 RAM）
psram:         0 B
```

版本串 `AI Watch v4.8.25-persist` 已进 bin；新日志串
`aiwatch-ble: INIT_FAILED, auto-recover attempt %d (next in %lu s)` 已在 bin 内确认。
产物留存：`cmake_out/bins/4.8.25-persist.bin`（2,540,524 B）。

**SRAM 未变**这一点很关键：邮箱窗口（`0x2007FC00-0x2007FFFF`）与堆布局没有被动过，
不触发 ble.md「稳定性修复的硬依赖」里的内存布局风险项。

## 4. 验收表（待上板）

| 项 | 判据 | 结果 |
|---|---|---|
| 构建 | 0 error，版本串正确 | 通过 |
| SRAM 布局 | 与 p02 同值 311,316 B | 通过 |
| BLE 回归 | 连接→订阅→校时→提醒 | **待验证** |
| 失联自愈 | 制造 INIT_FAILED 后不再停（日志 attempt 持续增长） | **待验证** |
| 阶梯 | 日志能看到 30/60/120/300 的间隔变化 | **待验证** |
| 采样/UI | 15Hz、触摸/KEY2 正常 | **待验证**（未动相关代码） |

## 5. 上板步骤

```bash
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 --before default_reset --after soft_reset \
  write_flash /home/ma/openvela-contest403/cmake_out/bins/4.8.25-persist.bin@0x12010000
```

验证要点：
1. 启动日志出现 `AI Watch v4.8.25-persist`；
2. 正常连接一次，确认链路无回归；
3. 人为制造 init 失败（例如连上后快速断连多次，或临时在 `ai_watch_ble_init` 返回前注入
   失败——仅调试用），观察日志 `auto-recover attempt` 超过 5 次仍在增长；
4. 确认 attempt 6 之后间隔变成 300s，且期间主循环无卡死（触摸/翻页正常）。

## 6. 未做 / 边界

- 未改 `resume_advertising` 的 30s 退避（属桥接层，动它要重跑 BLE 全链路 A/B）。
- 未改 `ai_watch_ble_bsp_recover()` 本身。
- 未提交、未推远端（按约定等用户点名）。
