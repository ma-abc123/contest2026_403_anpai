# 开发日志 2026-11-03c — BLE 启动延迟：LCPU blob 替换 A/B 定案

## 结论（先说结果）

1. **换回官方原版 LCPU blob 没有修好启动问题。** 同条件 8 轮广播彩票：官方原版 2/8、E1 反编译版 2/8、pre_e1 3/8、4.8.73 基线 2/8、历史 4.8.56-ui5 镜像 3/8 —— 全部落在同一分布，n=8 下无显著差异。按用户指示「修不好不用在这浪费太大时间，留着也行」，**保留 E1 反编译 blob 原状**，未向公共仓提交任何改动。
2. **「重刷 4.8.56-ui5 可直接恢复」经 8 轮实测不成立（3/8）。** 此前是单次观察；项目既有结论「单次烧写无证据力」再次被验证。启动失败是**跨应用版本、跨 blob 版本稳定存在的概率性 LCPU bring-up 超时**，不是 4.8.56 之后某次应用变更或 blob 修改引入的回归。
3. **启动延迟的代码级根因未找到。** 未经实机 A/B 证实的原因一律标为假设，不声称已修复。
4. 4.8.73 已验证的 TX 串行化修复（`ai_watch_ble.c`）**一字未动**；恢复阶梯 15/20/30/60/120 s 未改；参赛仓 8 个未提交修改全部保留。

## A/B 矩阵（统一条件：ble_adv_lottery.py -n 8 --scan-s 12，测试前停 ai-watch-viewer）

| 镜像 | 应用代码 | LCPU blob | 广播可见 | 日志 |
|---|---|---|---|---|
| `bin_4873_test_bletx_serial.bin` | 4.8.73（用户现场） | E1 | 2/8（首轮基线） | — |
| `bin_4877_e1_control.bin` | 当前源码 | **E1**（HEAD 原状） | **2/8** | `lottery_4877_e1.log` |
| `bin_4878_official_blob.bin` | 当前源码 | **pre_e1**（命名误导，见下） | **3/8** | `lottery_4878_official.log` |
| `bin_4879_official_true.bin` | 当前源码 | **官方原版** | **2/8** | `lottery_4879_official_true.log` |
| `bin_4856_ui5_anchor.bin` | 4.8.56 锚点（旧） | **E1**（二进制验证） | **3/8** | `lottery_4856_anchor.log` |

镜像 SHA-256：

- E1 控制 `38262d6b24593f3599f4ec0a4396bdcdeff658d62735363870f3d8cd4b806074`
- pre_e1 `21aee46f06faba6dd147d079ae0d8ee7896039d678aecd307f0893426e5c2eee`
- 官方原版 `df87ff9bf38d55ee483341b30b35bc599f260185160c3da7d3ffd43760336657`
- 4.8.56 锚点 `2752f9166532b6b8083e7fbea05502d3153b15e0282ad433b0ccc74768119c0a`
- 用户现场 4.8.73 `f3dc3340ef0937edc5044ea0d688d0952fb3893be9a6df98d556046b29dfa0b5`

### 一次需要纠正的命名错误

上一轮把 `_exp.h` 换成 `.pre_e1` 后构建的 `bin_4878_official_blob.bin`，名字里的 "official" 是**误导**：`.pre_e1` 是 E1 之前的一版反编译 blob，不是官方原版。真正的官方对照是 `bin_4879_official_true.bin`。已用二进制特征字节验证四个镜像内实际装载的 blob：

- 官方 `sf32lb52_lcpu_patch.h`：`g_lcpu_patch_bin` 2024 words
- pre_e1 / E1：2040 words（新版 SDK blob），两者仅 idx 1164–1171 共 8 个 word 不同
- **E1 的「反编译修复」本质是把 idx 1164–1171 八条指令全部改成 `0xbf00bf00`（双 NOP）**；官方对应位置是 8 条真实指令（`f894f9e7 / e7ef5100 / bf00bd70 / 40090000 / …`）
- 二进制验证：`nuttx.bin`(重建)=E1、`bin_4877`=E1、`bin_4879`=official、`bin_4856`=E1

### 单变量控制方式

保持 `sf32lb52_lcpu_boot.c` 不变（仍 include `_exp.h`），只把 `_exp.h` 内 `g_lcpu_patch_list` / `g_lcpu_patch_bin` 两个数组逐字替换为官方 `sf32lb52_lcpu_patch.h` 的值，保留 include guard 与注释头。这样唯一变量是 blob 内容，避免直接改 boot.c include 时引入 `_b` 数组等无关差异。每次构建前删除 `sf32lb52_lcpu_boot.c.o` 强制重编。

运行时日志 `lcpu patch: revid=3 -> legacy patch` 确认走 `lcpu_patch_install_legacy()`，数组被宏重命名为 `_legacy` 后加载，**改 `_exp.h` 确实生效**，A/B 方法有效。

## 失败形态（所有镜像一致）

```
<wrn> Force teardown: closing HCI driver
aiwatch-ble: force host teardown result=0
dev:0: Sending HCI_RESET
lcpu patch: revid=3 -> legacy patch
lcpu assert: flag=0 name=0x00000000 line=0
cal: lcpu release
sf32lb52 bt IPC queue armed before LCPU release
aiwatch-ble: bt_enable failed: -110        （或 advertising start failed: -110）
```

少数轮次出现 `lcpu assert: flag=<乱码>`（读到未初始化内存），说明该 assert record 的读取本身不可靠，不宜作为判据。还观察到板端打印 `advertising as "AI-Watch-403"` + `ready, GATT service exposed` 但主机 `discovered=False` —— **板端自认为广播成功 ≠ 主机能扫到**，不把 ADV 成功当连接成功。

## 应用侧差异核对（对应求助提示词第 1 问）

`anchor-ui5-20261101..HEAD` 共 6 个文件、+346/-28：

- `ai_watch_main.c`：新增 `g_lcd_strip[W*100]` 大静态缓冲、`g_lcd_shadow` 改为按 strip 行 `lv_malloc` 分档、投屏/录制相关初始化
- `ai_watch_motion.c/.h`：慢采样、录制缓冲与状态机
- `ai_watch_shot.c/.h`：投屏链路调整
- `defconfig`：+`CONFIG_LV_USE_SYSMON`、+`PERF_MONITOR`、+`LV_LOG_PRINTF/INFO`、+`-O3`

**这些差异不能解释首次 `0x0c03` 超时**：4.8.74（全帧 PSRAM）、4.8.75（栈回 8 KB）、4.8.76（关 LVGL sysmon/perf/INFO）此前均未修好；本轮整机回退到 4.8.56 镜像仍是 3/8。未发现可解释首次超时的启动前调用顺序、线程优先级/栈、SRAM heap 或动态分配顺序差异。**应用侧差异作为根因已被实机排除。**

## 未证实的假设（不是结论）

- **H1（当前最强）**：LCPU release 前 RF/MAC 未就绪 → 首个 HCI_RESET(0x0c03) 无响应 → `-110`。依据：`sf32lb52_lcpu_boot.c:176-203` 已有注释「every first 0x0C03 timed out … Give the RF/MAC a settle window」并实现了 100 ms settle + 等待 MAC+0x00 bit31 清零；但失败日志**只有 `cal: lcpu release`，没有 `cal: begin` / `cal: end mac0=` / `cal: settled mac0=`**，说明 `g_lcpu_rf_cal_disable != 0`，这段保护被整体跳过。**未查清**：boot.c 内 `lcpu_disable_rf_cal()` 没有调用点，为何该 static 变量非 0 尚未确认（可能与 `chips/drivers/cmsis/sf32lb52x/bf0_lcpu_init.c` 的同名函数/链接覆盖有关）。
- **H2**：首轮基线中出现两次 `sem_waitirq.c:137` 内核断言（`bt_hci_cmd_send_sync` 路径），提示 host 侧 HCI 同步命令在超时/teardown 时被破坏，属并发缺陷而非单纯等待。**未证实**。
- **已排除**：blob 内容（三个 blob 差异从 8 到 1490 words，结果一致）、4.8.56 之后的应用变更、运动线程栈大小、LVGL 诊断开关、恢复阶梯长度。

## 连接回归为何未做

用户要求「不要把能广播当成能连接」。本轮所有候选在 ADV 层面均无显著改善，按「修不好不用在这浪费太大时间」的指示，**未跑** `ble_connect_probe.py` / `ble_connect_test.py`。4.8.73 的 TX 串行化修复未被改动，用户此前实测连接成功，故无需重验；但本轮也不声称连接层面有任何新结论。

## 后续可选（仅列，未执行）

1. 最小单变量：在 `lcpu_ble_patch_install()` 去掉 `if (g_lcpu_rf_cal_disable == 0)` 的 gate，强制走 100 ms settle + MAC bit31 等待；不改 blob、不动恢复阶梯、不动 TX 串行化，再跑 8 轮彩票。这是验证 H1 成本最低的一步。
2. 仅当 ADV A/B 出现显著改善时再跑连接回归。

## 现场恢复状态（收尾已确认）

- `vendor/sifli/middleware/bluetooth/patch/sf32lb52/sf32lb52_lcpu_patch_exp.h` 已还原为 E1，哈希 `6104cb58…`，`git status` 中 `M` 已消失，只剩原有的 `.bak` / `.pre_e1` 两个 untracked 备份。
- `cmake_out/prod/nuttx.bin` 已用 E1 重建并经二进制验证（重建 sha `0e7ad26e…` 与首轮 `38262d6b…` 不同，属 NuttX allsyms/链接顺序非确定性，blob 内容一致）。
- 板子已烧回用户指定的 `bin_4873_test_bletx_serial.bin`（`f3dc3340…`）。
- `ai-watch-viewer` 已恢复 `active`。
- 参赛仓 `exp/capture-panel` 的 8 个已修改文件与全部未跟踪文件保持原样。

## 备份素材（均在 ~/huangshan-logs/，未入仓）

- `sf32lb52_lcpu_patch_exp.e1.20261103.h`（E1 原版，6104cb58…）
- `sf32lb52_lcpu_patch_exp.official_20261103.h`（官方原版等价 _exp.h，890e800c…）
- `lottery_4877_e1.log` / `lottery_4878_official.log` / `lottery_4879_official_true.log` / `lottery_4856_anchor.log`
- `build4878_official.log` / `build4879_official_true.log` / `build_restore_e1.log`
