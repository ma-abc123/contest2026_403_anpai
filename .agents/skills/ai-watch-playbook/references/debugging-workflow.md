# 板端排查方法论：从串口日志到根因

本项目的多个硬根因全部来自**对串口日志数字的读取**，不是猜测。流程与实例：

## 采集

- picocom `--logfile` 落盘到 `~/huangshan-logs/ai-watch-YYYYMMDD-HHMMSS.log`；
  串口 1M 波特。
- 固件日志行必须自带证据：可 grep 前缀（`REC,` / `MOTION:` / `BLE:` /
  `aiwatch-ble:`）+ 关键数字（时间戳、字节数、角度、mg）。
- 时间统一 `CLOCK_MONOTONIC` 毫秒，跨线程可比。

## 分析套路（按序）

1. **区分错误层**：应用 printf → host 层（zblue `<wrn>/<err>`）→
   控制器层（`Hardware error` + `lcpu evidence`）。层不同，责任方不同
   （见 references/ble.md）。
2. **看节奏不只看内容**：把日志行的时间戳差值列出来找周期。实例：
   - REC 时间戳 `40,43,172,40,40,183…` —— 每第 4 条（每条 BLE 帧）一个
     ~120ms 台阶 → `bt_gatt_indicate` 同步阻塞；
   - 连接后 ~50 条/秒的 "not subscribed" → 失败后无退避的循环重试。
3. **看缺什么**：预期出现的行没出现（如没有第二次 `impact` 行）= 被
   状态/冷却/门限挡住，去对照状态机而非盲加日志。
4. **A/B 对照固件**：怀疑某改动时，新旧两版 bin 只差该改动，各跑同一
   复现脚本（瘦身期 Hardware Error 的排查范式）。
5. **交叉验证编译错误**：报错看最后一行；GCC 的 "did you mean" 建议与
   exit code 互相印证；先区分 vendor 老代码（EXTRA_FLAGS 放行）与真 bug。
6. **升级给专家前**：把 evidence（revid/补丁路径/assert 记录/配置字，
   `lcpu_boot_dump_evidence` 打印的那几行）、复现率、已排除清单、时间线
   整理成单页文档（模板见 `docs/development/BLE连接崩溃排查提示词-2026-09-02.md`）。

## 验证纪律

- 每个功能修完跑回归清单（日志里"实机验证清单"节是模板）。
- "触摸死、按键活" → 主循环被阻塞（见 SKILL.md 红线 1）。
- 启动日志哨兵：heap arena 大小（邮箱窗口）、`lsm6dsl0 registered`、
  `advertising as`、版本号——四行齐了才算正常起跑。
- 结论落开发日志：现象、根因、修复、验证证据、未验证声明，一样不缺。
