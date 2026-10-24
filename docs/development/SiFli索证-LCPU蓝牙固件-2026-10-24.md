# 向 SiFli / 立创社区索证材料：SF32LB52 LCPU 蓝牙固件随机致命（2026-10-24）

> 用途：直接发帖或工单。8-30 会诊时已落地 evidence 采集（lcpu_boot_dump_evidence），
> 本文档汇总当日复现的全部数据。

## 硬件与环境

- 板子：立创·黄山派（SF32LB52，**revid=3**），USB 供电（充电头 2A 与 PC 口均复现）
- 主机栈：openvela（NuttX）+ zblue 移植；LCPU 补丁 blob 为上游同步版
  （vendor e1cc823 2026-08-30 同步上游 HEAD；9bf18ff 修正 legacy 路径加载该 blob，
  09-02 实机验证通过）
- 与 HCPU 固件配置无关：A/B 测试（胖配置 4.8.5 vs 瘦身 4.8.6+）同款死法

## 症状（2026-10-24 实测，串口日志为证）

1. **静默僵死（无 Hardware Error 事件）**：控制器 bring-up 中在广播配置序列的命令上
   超时（`Controller unresponsive, command opcode ... timeout with err -11`）。已观察到的
   opcode：0x2005（LE Set Random Address）、0x2036（LE Set Extended Advertising
   Parameters）、0x2038（LE Set Extended Scan Response Data）。当日每次控制器 bring-up
   （含自愈重启后的 LCPU）约 50% 概率发生，僵死后该控制器不再应答任何 HCI 命令。
2. **连接建立即死**：广播正常、手机连接成功后 ~1s 内
   `Hardware error, hardware code: 0`（事件 0x10）。

## evidence dump（每次重启前采集，多次样本）

```
revid=3 (legacy patch path) assert_over=2
ai=54b2f5ef d0fd15a1 aecb3edf 053c5c0f   （另两次：54b6f5eb d0f915a1 aacb3edf… /
                                           d4f7f5ef d0fd15a1 aacb3edf…）
cfg=45457878 00000000 00000000 0000000a
```

## 已排除

- HCPU 堆/配置变化（A/B 两版固件同款）；
- 主机 HCI 流量异常（08-30 DBG 日志：命令全 0x00、分片远小于窗口）；
- 供电（2A 充电头与 PC 口同款）；
- 共享窗口保护缺失（0x500 字节保留已在位）；
- （初步排除）flash NVDS 损坏：SRAM NVDS 每次 enable 由主机重写默认值，
  RF 校准在 LCPU release 前全流程执行。

## 自愈现状

手表侧已实现 controller_restart（LCPU reset/halt（HAL_RCC_Reset_and_Halt_LCPU，
非电源域断电）→ 重建邮箱 → 重装补丁 → 新执行上下文）+ 主机栈
重建，能把大多数失败拉回可用；但全新 LCPU 仍以 ~50% 概率在同一命令族僵死，
无法根治。

## 想请教的问题

1. revid=3 + legacy patch 路径的 LCPU 蓝牙固件，在广播配置序列（LE Set Random
   Address 0x2005 / LE Set Ext Adv Parameters 0x2036 / LE Set Ext Scan Rsp Data
   0x2038）与连接建立处随机僵死，是否有已知 issue 或更新版补丁 blob？
2. `assert_over` 的语义确认与 `ai=`（0x2040FDA0 起四字）的字段定义和解码方法：
   本地读出 HAL_LCPU_ASSERT_INFO_get() 返回 HAL_BUSY(=2)，即 0x2040FDBC 完成标志
   ≠0xa5a5a5a5——我们据此理解为"assert 完成标记未被观察到"，请确认并给出
   ai 四字的解析方式（PC/LR/断言行号？），以便我们提供更精确的现场；
3. 建议的 RF 校准/NVDS 数据自检方法（排除校准数据损坏的可能）。
