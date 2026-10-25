# 2026-10-26 凌晨：r7/r8——钩子证伪、legacy 证伪、E1+快速自愈成为交付形态

接 2026-10-25 的 r6 结论（HW error 根因 = 钩子 RFC 复位脉冲，剂量效应成立）。
本轮目标：按分析方 r7/r8 方案收口第二故障模式（wedge：LCPU 消费命令后
CC 永不返回，init/重广播序列中随机命中，自愈 100% 但要 10-20s）。

## 实验记录

| 构建 | 变体 | 结果 |
|---|---|---|
| 4.8.15-c2e4 | E4（去脉冲+去两等待，r7 首选） | wedge 仍在（0x2035/0x2039） |
| 4.8.15-c2e6 | E6（钩子整体 no-op） | **wedge 仍在（0x2037/0x1003）→ 钩子证伪** |
| 4.8.16-c2e1 | E1 + 采样器读 H2L | 0x0C03×3 全超时——**host bug**（见下） |
| 4.8.16-c2e0 | E0（去脉冲+去 MAC 死等） | bring-up 0 超时；5 连接全成（10-328s）；HW error 1 次；wedge 11 次（自愈 100%） |
| 4.8.17-e1-legacy | r8 A：EXT_ADV=n + E1 | legacy 生效但 wedge 挪到 0x2008/0x1001 → **opcode 无关证伪** |
| 4.8.18-e1-legacy | + 快速自愈 | **在板交付候选**；wedge 0x200A，恢复 ~4-5s |

## 关键根因记录

1. **wedge 不在厂商钩子里**：E6 把整个 0x5938 钩子 NOP 掉仍然 wedge。
   r6 的"钩子内三个无限等待"理论证伪。
2. **wedge 与 opcode 无关**：r8 legacy 广播换掉整个扩展广播命令族，
   wedge 出现在 0x2008/0x200A（legacy）与 0x1001（纯协议）。
   跨轮共 10 个不同 opcode 命中。LCPU 在 init/广播配置时间窗内整体停机。
3. **c2e1 事件（我方 bug）**：采样器对 H2L 环头（0x2007FE08）做
   `up_invalidate_dcache`，把 bridge 环初始化尚未 clean 的脏行丢掉，
   LCPU 读到垃圾环头（buffer_size 488≠492，dump 实证），0x0C03 全超时。
   教训入 playbook 红线 #13：mailbox 窗口严禁 HCPU 侧 invalidate。
4. **A3 硅片无睡眠配置通路**：`HAL_LCPU_CONFIG_BT_CONFIG`（含
   default_sleep_enabled）只在 rev≥A4 的 rom_array_a4 里；A3 的 0x40 字节
   配置表写入该类型会被丢弃（dump 证实 bt_rom_config 全零）。关睡眠需
   NVDS 标签（ROM 内，未解）。

## 交付形态（4.8.18-e1-legacy）

- blob E1：去掉钩子的 RFC 复位脉冲（HW error 根因修复，78%→0/4）。
- legacy 广播：绕开扩展广播命令族（wedge 未修但命令更少；EXT_ADV=n
  需连带关 BT_SCAN_AND_INITIATE_IN_PARALLEL，否则被 select 回来）。
- 快速自愈：zblue HCI 命令超时 10s→3s（zblue 11ec7e76b31）；LPS 全量
  dump 限每 boot 前 2 次故障（contest e990003）。恢复周期 15-20s→4-5s。
- 残余已知问题：wedge（约每 5-10 分钟广播态一发/断开后重广播多发，
  自愈 4-5s）；E1 下偶发 Hardware Error（r7 一晚 1 次，来源未定）。

## 验收

- 冷启动：wedge 概率仍 ~50-80%/init，但 3s 检出 + 4-5s 自愈，开机
  最坏 ~15s 到 ready（此前 30-60s）。
- 连接（c2e0 数据）：5/5 成功，10s-328s 保持，timesync 正常。
- 待补：50 次长跑 + 10 分钟连续连接（分析方 TEST_PLAN_R8 §4）。

## 遗留

- SWD halt 拿 wedge 现场 PC 是唯一剩余的一次定死测量（需 DAP-Link）。
- 分析方 ROM 静态分析：LCPU 事件循环的无命令依赖停机条件。
- E1 生产确认（射频长期一致性）走 fork PR 流程。
