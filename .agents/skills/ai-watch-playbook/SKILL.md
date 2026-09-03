---
name: ai-watch-playbook
description: AI Watch（openvela/NuttX + LVGL + zblue，立创黄山派 SF32LB52）开发手册——从本项目 12 篇开发日志提炼的红线规则与领域模式。凡在 openvela-contest403 工作区开发、或涉及 SF32LB52/黄山派/NuttX 固件、LVGL UI、zblue BLE、I2C/传感器驱动、跌倒检测算法、板端串口日志分析的任务，先读本 skill 再动手；尤其改 ai_watch、写驱动、调 BLE、分析板端日志时必读。
---

# AI Watch 开发手册（从开发日志提炼，2026-08-23 ~ 09-02）

来源：`contest2026_403_anpai/docs/development/` 下全部开发日志。每条规则背后
都有一次真实翻车，按任务域读对应 `references/` 文件，不要凭感觉跳过。

## 红线（每条都付出过代价）

1. **LVGL 主循环里不做任何阻塞 I/O**（I2C / UART / BLE 同步调用）。
   主循环每 ~10ms 一转，一次 1-2s 的阻塞 = 触摸全死而 KEY2（GPIO 轮询）正常。
   "触摸死、按键活"的唯一常见解释就是主循环被阻塞，见到先往这个方向查。
   慢操作一律：后台 pthread + mutex 快照 + 主循环 process() 消费。
2. **对不在线的 I2C 设备做一次探测 = 阻塞 1-2s**：SiFli HAL 的
   `I2C_TIMEOUT_ADDR/BUSY` 各 1000ms。探测永远放后台线程，用
   PENDING/OK/ABSENT 三态 + 100ms 切片 sleep 保证可 join，absent 后 5s 退避。
3. **`bt_gatt_indicate` 在本移植上同步阻塞约一个连接间隔（~100-150ms/次）**。
   一切周期性上报必须走独立 TX 线程 + 队列 + 失败退避 2s；首发失败禁止在
   循环里重试（曾刷出 119 条 "Device is not subscribed" 内核告警）。
4. **改代码必 bump 版本号（AI_WATCH_VERSION），烧录前必备份 bin**
   （`size_slim_20260829/final_XXX_nuttx.bin`）。版本号忘了 bump，串口日志
   与固件对不上，排查必走弯路（4.8.2→4.8.3 期间实际发生过）。
5. **算法先离线后上板，分析脚本必须与固件同源**：状态机规则、初值、单位、
   采样率假设全部镜像。两边不同源时离线结论会骗人（跌倒角度基准 bug 因此漏网）。
6. **串口日志是主调试通道**：日志行带可 grep 前缀和数字（`REC,...`、
   `MOTION: ...`、`BLE: ...`），时间用单调钟毫秒。本项目多个根因（120ms 节奏、
   119 条告警、34° 角度）都是从日志数字里读出来的，方法论见
   references/debugging-workflow.md。
7. **LVGL 不是线程安全**：一切 UI 调用只在主循环线程；后台线程只写共享数据，
   由主循环渲染。对象所有权与页面生命周期规则见 references/lvgl-ui.md。
8. **git 安全**：禁 `git reset --hard` / `git checkout --` / 随意
   `repo sync`；提交由用户主导；apps、vendor/sifli、contest 仓是**独立
   git 仓**，提交要分仓做。
9. **诚实标注**：没实机验证的功能必须写"未验证"，不假装；开发日志按日期
   放 `docs/development/`，必须含根因记录与验收表。
10. **报错要看到最后一行再下结论**；vendor 老代码的编译错误先用
    `-Wno-error=*` 四件套 + `-D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS`
    判别，见 references/build-flash.md。

## 按任务域读参考文件

| 任务 | 先读 |
|---|---|
| 编译 / 烧录 / 版本与备份 / 新终端环境 | references/build-flash.md |
| LVGL 页面 / 菜单 / 图标 / 字体 / 条幅 / 线程规则 | references/lvgl-ui.md |
| I2C 传感器 / 后台探测 / DHT22 / 引脚归属 | references/sensors-i2c.md |
| BLE 桥接 / 协议设计 / 上报 / 自愈 / 控制器崩溃 | references/ble.md |
| 跌倒 / 计步算法与阈值调优 | references/fall-detection.md |
| 板端问题排查（从日志找根因） | references/debugging-workflow.md |

## 项目速查

- 仓库：`/home/ma/openvela-contest403/contest2026_403_anpai`（参赛仓，应用在
  `app/ai_watch/`，经 linkfile 映射到 `packages/demos/contest2026_403_ai_watch/`；
  板级在 `board/ai_watch/configs/ai_watch/defconfig`，映射到
  `vendor/openvela/boards/contest2026_403_ai_watch/`）
- 构建树：`cmake_out/contest2026_403_ai_watch`；串口日志：`~/huangshan-logs/`
- 原始开发手册：`docs/development/SF32LB52_openvela_dev-ai-contest-2026_开发流程.md`
