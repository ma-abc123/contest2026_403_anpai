# 编译、烧录、版本与备份

## 环境（每次新终端）

```bash
export PATH="$HOME/opt/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin:$PATH"
```

- GCC 10.3 编不了本分支（vendor libc++ 是 LLVM 17，需要 GCC≥12 内建 trait），
  **必须用 Arm GNU 14.2.Rel1**。
- `repo sync` 不会拉 LFS：缺 lib 时到 `vendor/sifli/boards/sf32lb52/libs` 执行
  `git lfs pull`。
- 公共仓补丁需手动 `git am`（frameworks/runtimes/feature、vendor/sifli）。
- Ubuntu 的 brltty 会抢 CH340 串口，装完系统先卸掉。
- 工程有 200+ 嵌套仓库，改码前先 `repo status` / 目录内 `git status` 确认归属。

## 编译

```bash
# openvela 工作区根目录（参赛仓的上一级）
cmake -B cmake_out/contest2026_403_ai_watch -DNUTTX_BUILD=y \
  -DBOARD_CONFIG=../vendor/openvela/boards/contest2026_403_ai_watch/configs/ai_watch \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations \
   -Wno-error=implicit-function-declaration -Wno-error=int-conversion \
   -Wno-error=incompatible-pointer-types -Wno-error=return-mismatch \
   -D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS"
cmake --build cmake_out/contest2026_403_ai_watch --parallel 4
# 产物 cmake_out/contest2026_403_ai_watch/nuttx.bin；全量 ~2 分钟(-j4)
```

- EXTRA_FLAGS 不是可选的：GCC 14 把 vendor 老代码的隐式声明等升级成错误；
  四件套放行后仍报错的才是真 bug（全工程仅一个源码 bug：feature_utils.h:212
  move 构造多了 const）。
- **menuconfig 是唯一不可自动化的步骤**——优先直接改 defconfig 再重配。
- 栈大小两个变量别混：`CONFIG_INIT_STACKSIZE` 管 init 入口任务，
  `CONFIG_EXAMPLES_AI_WATCH_STACKSIZE` 只管 `task_create` 的任务；
  `CONFIG_DEFAULT_TASK_STACKSIZE` 不影响 init。LVGL 要求应用栈 ≥32768。

## 烧录与串口

```bash
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 \
  --before default_reset --after soft_reset \
  write_flash <绝对路径>/nuttx.bin@0x12010000
```

- **烧录路径必须绝对路径**，sftool 解析相对路径会失败。
- 串口 1M 波特；picocom 用 `--logfile` 养成落盘习惯（日志进 `~/huangshan-logs/`）。
- 开机黑屏 ≠ 坏（Lyra/应用加载要几秒）；fb 例程的全屏淡粉是 RGB565 0xEC1D
  首帧，不是故障——先解码颜色再判断对错。
- 旧配置 Lyra 会抢屏：NSH 里 `kill -15 <pid>`；AI Watch 配置已按配置不启 Lyra。

## 版本与备份惯例（纪律）

1. 改代码必 bump `AI_WATCH_VERSION`（ai_watch_main.c），发布串口会打印。
2. 每个验证通过的固件备份：`cp nuttx.bin size_slim_20260829/final_XXX_nuttx.bin`。
3. 版本号忘 bump 的代价：串口日志版本与实际固件不符，排查方向全错。
4. 提交分仓：apps、vendor/sifli、contest2026_403_anpai 是独立 git 仓；
   提交前过 `checkpatch.sh`（文件头路径以 apps/ 开头；大括号与 if/else 对齐）。
5. 主循环启动时打印 heap arena 大小——它必须低于 LCPU 邮箱窗口
   （0x2007FC00），否则 BT 流被堆覆盖（见 references/ble.md 的邮箱窗口一节）。
