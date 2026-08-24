# 立创·黄山派 SF32LB52 openvela `dev-ai-contest-2026` 开发手册

> 适用目标：基于立创·黄山派 SF32LB52 和 openvela 实现 AI 腕上助手。
> 开发方式：以 AI coding 为主，每次只交付一个可验收的功能。
> 已核对日期：2026-08-23。远程分支会继续变化，首次成功同步后应导出锁定 manifest。

## 1. 先看结论

1. 黄山派必须使用 `dev-ai-contest-2026` manifest，保证 `nuttx`、`apps`、`vendor/sifli` 以及 SF32LB52 预编译库来自相互匹配的竞赛分支。
2. openvela 源码要放到**不包含中文和空格**的路径。本手册统一使用：

   ```text
   /home/ma/openvela-sf32lb52
   ```

3. 直接在新源码树完成“编译 -> 烧录 -> NSH -> LCD/LVGL”基线。
4. 业务代码放在 `apps`，黄山派板级初始化放在 `vendor/sifli`，通用 NuttX 驱动才放在 `nuttx/drivers`。不要修改 `cmake_out` 内的生成文件。

## 2. 工作区约定

项目源码统一放在 `/home/ma/openvela-sf32lb52`。工作区中的 `.agents/`、`.claude/` 和 `.codex/` 仅提供开发工具与资料，不参与固件编译；`cmake_out` 只存放构建生成物，不要直接修改其中的文件。


## 3. 开发机环境

### 3.1 当前机器检查结果

2026-08-23 实际检查：

| 项目 | 当前状态 | 处理 |
|---|---|---|
| Ubuntu | 22.04 x86_64 | 符合 quickstart 要求 |
| 内存 | 14 GiB，可用约 10 GiB，15 GiB swap | 同步和编译建议 `-j4` |
| `/home/ma` 剩余空间 | 约 52 GiB | 可以开始，但要保持至少 15-20 GiB 余量 |
| Git | 已安装 | 无需处理 |
| Repo | `/usr/local/bin/repo` | 已安装 |
| Git LFS | 已安装 | 需执行 `git lfs install` |
| CMake | 3.22.1 | 符合最低 3.22 |
| Python | 3.10.12 | 符合最低 3.10 |
| Kconfiglib CLI | `olddefconfig/menuconfig/savedefconfig` 未安装 | 必须安装，否则 CMake 配置直接失败 |
| Ninja | 未安装 | 必须安装 |
| `arm-none-eabi-gcc` | 未安装 | apt 的 10.3 编不过本分支，必须按 3.2 装 Arm GNU Toolchain 14.2 |
| `genromfs` / `xxd` | 已安装 | ROMFS 构建工具 |
| Picocom | 未安装 | 必须安装 |
| `sftool` | 未安装 | 烧录前安装 |
| Cargo | 未安装 | 可选；使用预编译 `sftool` 时不需要 |

### 3.2 安装缺失依赖

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  curl \
  genromfs \
  git \
  git-lfs \
  ninja-build \
  picocom \
  python3 \
  python3-pip \
  python3-serial \
  xxd \
  xz-utils

git lfs install

python3 -m pip install --user kconfiglib pyelftools cxxfilt devicetree
export PATH=/home/ma/.local/bin:$PATH
```

注意：**不要**用 `apt install gcc-arm-none-eabi` 提供交叉编译器。Ubuntu 22.04 源里是 10.3.1，编不过本分支的 LLVM 17 libc++（原因见 5.1.1）。交叉编译器按下一节安装官方 14.2 版。

检查最低版本：

```bash
arm-none-eabi-gcc --version | head -1
cmake --version | head -1
ninja --version
python3 --version
repo --version
git lfs version
command -v olddefconfig
command -v menuconfig
command -v savedefconfig
command -v genromfs
command -v xxd
python3 -c "import elftools, cxxfilt, devicetree, yaml"
```

### 3.2.1 安装 Arm GNU Toolchain 14.2

本分支的 C++ 标准库是 LLVM 17，要求构建编译器 GCC ≥ 12。统一使用 Arm 官方 14.2.Rel1（思澈/立创生态实际使用的版本）：

```bash
mkdir -p ~/opt && cd /tmp/opencode
curl --fail --location --output \
  arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz \
  https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz
curl --fail --location --output toolchain.sha256asc \
  https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz.sha256asc

sha256sum --check toolchain.sha256asc   # 必须输出 OK
tar -xf arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz -C ~/opt/
```

解压约 1.5 GB。把下面两行写入 `~/.bashrc` 并重开终端：

```bash
export PATH="$HOME/opt/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin:$PATH"
export PATH="$HOME/.local/bin:$PATH"
```

验证：`arm-none-eabi-gcc --version` 应显示 **14.2.1**。若系统还残留 apt 版 10.3，绝不能让它排在 PATH 前面。

`nuttx/CMakeLists.txt` 会在配置初期直接查找 `olddefconfig`。如果 `pip` 安装成功但命令仍找不到，在当前终端执行：

```bash
export PATH=/home/ma/.local/bin:$PATH
```

重新登录后 Ubuntu 通常会自动将 `/home/ma/.local/bin` 加入 `PATH`。编译前仍应使用 `command -v olddefconfig` 确认。

黄山派官方板级 README 要求：

| 工具 | 最低版本 |
|---|---|
| `arm-none-eabi-gcc` | 10.3 |
| CMake | 3.22 |
| Ninja | 1.10 |
| Python | 3.10 |

> **2026-08-23 实测修正**：表中“gcc ≥ 10.3”对本分支不成立。vendor 的 libc++ 是 LLVM 17.0.6，GCC 10 缺少必需的内建 trait，编译直接失败；实际需要 **Arm GNU Toolchain 14.2.x**，详见 5.1.1。

### 3.3 安装 `sftool`

推荐使用官方预编译 x86_64 Linux 版，不需要 Rust/Cargo。使用唯一临时目录，并在安装前验证官方 SHA-256：

```bash
SFTOOL_TMP="$(mktemp -d)"
SFTOOL_ARCHIVE=sftool-0.2.5-x86_64-unknown-linux-gnu.tar.xz
SFTOOL_BASE_URL=https://github.com/OpenSiFli/sftool/releases/download/0.2.5

curl --fail --location \
  --output "$SFTOOL_TMP/$SFTOOL_ARCHIVE" \
  "$SFTOOL_BASE_URL/$SFTOOL_ARCHIVE"
curl --fail --location \
  --output "$SFTOOL_TMP/$SFTOOL_ARCHIVE.sha256" \
  "$SFTOOL_BASE_URL/$SFTOOL_ARCHIVE.sha256"

cd "$SFTOOL_TMP"
sha256sum --check "$SFTOOL_ARCHIVE.sha256"
tar -tf "$SFTOOL_ARCHIVE"
tar -xf "$SFTOOL_ARCHIVE"
test -f "$SFTOOL_TMP/sftool"

sudo install -o root -g root -m 0755 \
  "$SFTOOL_TMP/sftool" \
  /usr/local/bin/sftool

sftool --version
```

只有在 checksum 输出 `OK`、archive 中的文件名为 `sftool` 时才执行 `sudo install`。不要从可预测的 `/tmp/sftool` 直接安装，以免误用旧文件。

如果预编译包地址已更新，从 [OpenSiFli/sftool Releases](https://github.com/OpenSiFli/sftool/releases) 选择最新的 `x86_64-unknown-linux-gnu` 包。

将当前用户加入串口权限组：

```bash
sudo usermod -aG dialout "$USER"
```

执行后需要重新登录图形会话或重启，之后检查：

```bash
groups
```

输出应包含 `dialout`。

## 4. 同步竞赛分支源码

### 4.1 初始化新工作树

```bash
mkdir -p /home/ma/openvela-sf32lb52
cd /home/ma/openvela-sf32lb52

repo init \
  -u https://github.com/open-vela/manifests.git \
  -b dev-ai-contest-2026 \
  -m openvela.xml \
  --git-lfs
```

成功标志：

```text
repo has been initialized in /home/ma/openvela-sf32lb52
```

### 4.2 同步

当前机器有 14 GiB 内存，优先使用 4 并发：

```bash
cd /home/ma/openvela-sf32lb52
set -o pipefail
repo sync -c -j4 2>&1 | tee repo-sync.log
```

同步中断可以直接重复执行。如果频繁出现 `early EOF`、连接重置或内存压力，降为：

```bash
repo sync -c -j2
```

不要在首次网络错误时删掉整个目录；`repo sync` 可以续传。

### 4.3 验证源码完整性

```bash
cd /home/ma/openvela-sf32lb52

test -d .repo
test -f nuttx/CMakeLists.txt
test -d apps
test -d vendor/sifli/chips/sf32lb52
test -f vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md
test -f vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh/defconfig
test -d vendor/sifli/boards/sf32lb52/libs

git -C nuttx log -1 --oneline
git -C apps log -1 --oneline
git -C vendor/sifli log -1 --oneline
git -C vendor/sifli/boards/sf32lb52/libs log -1 --oneline
```

`vendor/sifli/boards/sf32lb52/libs` 是一个独立的 Git LFS 子仓库。目录存在不代表静态库已真正下载，需要额外验证：

```bash
SIFLI_LIBS=/home/ma/openvela-sf32lb52/vendor/sifli/boards/sf32lb52/libs

git -C "$SIFLI_LIBS" lfs pull
git -C "$SIFLI_LIBS" lfs fsck

for SIFLI_LIB in "$SIFLI_LIBS"/nsh_cmake/*.a; do
  test -s "$SIFLI_LIB"

  if grep -a -q '^version https://git-lfs.github.com/spec/v1' "$SIFLI_LIB"; then
    printf 'ERROR: LFS pointer was not downloaded: %s\n' "$SIFLI_LIB" >&2
    exit 1
  fi

  arm-none-eabi-ar t "$SIFLI_LIB" >/dev/null
done

repo status
```

`git lfs fsck` 和所有 `arm-none-eabi-ar t` 都成功，才能认定 SF32LB52 预编译库完整。

导出精确 commit 锁定文件，用于比赛提交和环境复现：

```bash
repo manifest -r -o manifest-lock.xml
```

建议将 `manifest-lock.xml` 和开发文档一起备份。

## 5. 第一次编译：不改任何代码

第一次的目标是证明官方基线可用。此时不要新增应用、不要改 defconfig、不要修驱动。

先确认 CMake 会调用的所有关键工具都在 `PATH` 中：

```bash
export PATH=/home/ma/.local/bin:$PATH

for OPENVELA_TOOL in \
  arm-none-eabi-gcc \
  cmake \
  genromfs \
  ninja \
  olddefconfig \
  python3 \
  savedefconfig \
  xxd; do
  command -v "$OPENVELA_TOOL" || exit 1
done
```

任何一项缺失都先回到第 3 章处理，不要带着不完整环境进入 CMake。

> **注意**：下面的命令是“官方原样”写法，在当前分支上会编译失败。实际可用的命令以 **5.1.3 节**为准（工具链 PATH + 扩展 EXTRA_FLAGS）。

```bash
cd /home/ma/openvela-sf32lb52

cmake \
  -B cmake_out/lckfb_huangshan_pi \
  -S "$PWD/nuttx" \
  -GNinja \
  -DBOARD_CONFIG=../vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"

cmake --build cmake_out/lckfb_huangshan_pi --parallel 4
```

检查产物：

```bash
test -s cmake_out/lckfb_huangshan_pi/nuttx.bin
ls -lh cmake_out/lckfb_huangshan_pi/nuttx.bin
file cmake_out/lckfb_huangshan_pi/nuttx
```

成功产物：

```text
/home/ma/openvela-sf32lb52/cmake_out/lckfb_huangshan_pi/nuttx.bin
```

官方 README 描述该文件约 1.5 MB。它是单一平面 XIP 镜像，没有需要另外烧录的 bootloader 或分区表。

### 5.1 实际构建记录与基线偏差（2026-08-23）

按上面原命令编译**失败**。逐一定位后确认了 3 类问题，全部有解且不需要大改源码。此后所有构建都必须沿用本节的工具链和 EXTRA_FLAGS。

#### 5.1.1 工具链：必须 Arm GNU Toolchain 14.2

apt 的 `gcc-arm-none-eabi` 是 10.3.1。本分支 vendor 的 C++ 标准库是 **LLVM 17.0.6**（见 `nuttx/libs/libxx/libcxx/libcxx/include/__config` 的 `_LIBCPP_VERSION 170006`），其头文件使用 `__is_nothrow_assignable` 等 Clang/GCC≥12 内建 trait，GCC 10 编译 libcxx 时直接报错。安装方法见 3.2.1。

#### 5.1.2 Python 依赖

构建脚本实际调用链需要的 pip 包比官方文档多：

| 包 | 缺失时的症状 |
|---|---|
| `pyelftools`、`cxxfilt` | `tools/mkallsyms.py` 生成 `allsyms_empty.c` 直接失败（ALLSYMS 符号表步骤） |
| `devicetree` | DTS 生成的 `gen_edt.py`/`gen_defines.py` 无法 import |
| `pyyaml` | 同上（edtlib 解析 binding 用） |

安装与自检命令见 3.2。

#### 5.1.3 EXTRA_FLAGS 必须扩展

GCC 14 默认将若干 C 历史遗留问题从警告升级为错误，vendor 芯片层代码会命中；libc++ 与 GCC 内建 trait 另有一处冲突。实测可用的完整配置命令：

```bash
export PATH=~/opt/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin:~/.local/bin:$PATH

cmake \
  -B cmake_out/lckfb_huangshan_pi \
  -S "$PWD/nuttx" \
  -GNinja \
  -DBOARD_CONFIG=../vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=return-mismatch -D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS"

cmake --build cmake_out/lckfb_huangshan_pi --parallel 4
```

各参数对应的问题（均已核实到具体文件）：

| 参数 | 解决的问题 |
|---|---|
| `-Wno-error=implicit-function-declaration` | `sifli_irq.c` 调用的 `arm_lowprintf` 定义在 `sifli_start.c` 且无任何头文件声明。符号链接期存在，只是 GCC 14 把隐式声明当错误 |
| `-Wno-error=int-conversion`<br>`-Wno-error=incompatible-pointer-types` | 同类 GCC 14 收紧项，vendor 老代码防御性放行 |
| `-Wno-error=return-mismatch` | `sifli_uart.c` 的 `up_putc` 按 `int` 实现并返回值，nuttx 头文件声明为 `void`；返回值被丢弃，无实际影响 |
| `-D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS` | GCC 声称支持 `__remove_pointer` 等内建 trait 却不允许用于函数签名（`__filesystem/path.h` 报错）；此宏令 libc++ 回退到标准库模板实现，仅涉及 3 个头文件 |

#### 5.1.4 一处 apps 源码修复（唯一源码改动）

`apps/frameworks/runtimes/feature/src/utils/feature_utils.h` 第 212 行 move 构造函数误写为 `const FeatureArray&&` 且函数体修改成员，属真正的源码 bug，任何标准 C++ 编译器都会拒绝：

```diff
-    FeatureArray(const FeatureArray&& other)
+    FeatureArray(FeatureArray&& other)
```

这是对整个源码树的唯一修改。它应当作为独立 commit 记录在 `apps` 仓库的工作分支上（见第 12 章），并在比赛材料中说明。

#### 5.1.5 结果与产物尺寸

- 编译成功，`cmake_out/lckfb_huangshan_pi/nuttx.bin` 约 **6.3 MB**，不是 README 写的 1.5 MB：该分支的 vendor commit（"pack HAP apps and font into romfs"）已把 QuickApp 和字库打进 romfs。
- flash 占用 38.8%（16 MB），SRAM 41.8%（512 KB），正常。
- 失败的历史构建目录按 `.gcc10-failed`、`.gcc14-permerr` 后缀归档在 `cmake_out/` 下，确认不再需要对比后可删除腾空间（每个约 2-4 GB）。

如果 CMake 缓存异常，不要删整棵源码树。只归档本板的构建目录：

```bash
cd /home/ma/openvela-sf32lb52
mv cmake_out/lckfb_huangshan_pi \
  cmake_out/lckfb_huangshan_pi.bad-cache
```

然后重新执行 CMake 配置和编译。

## 6. 硬件、烧录与串口

### 6.1 上电前检查

- 使用数据线连接 CH340N USB-UART。
- 确认电源跳线已接通：`5-6` 为 VBAT，`7-8` 为 VSYS，`11-12` 为 VCC_3V3。
- AMOLED 启动时电流可能较大。如果板子反复打印 `SFBL` 并重启，改用稳定的 5 V/2 A 供电或接入电池。
- CH340N 的 RTS 与芯片复位信号直连，低电平有效。

确认串口：

```bash
ls -l /dev/serial/by-id/ 2>/dev/null
ls -l /dev/ttyUSB*
```

从列表中核对 CH340N 设备，然后在**当前终端只设置一次**。以下只是 `/dev/ttyUSB0` 示例：

```bash
export SF32_PORT=/dev/ttyUSB0
test -c "$SF32_PORT"
udevadm info --query=property --name="$SF32_PORT" | sed -n '1,40p'
```

如果 `/dev/serial/by-id/` 存在稳定设备名，优先把 `SF32_PORT` 设为该精确路径。后续烧录和串口命令均复用这个已核对的变量，不再改回 `/dev/ttyUSB0`。这可以避免多个 USB 串口同时连接时复位或烧录错设备。

### 6.2 烧录

**固定目标地址是 `0x12010000`。**

```bash
cd /home/ma/openvela-sf32lb52
test -c "$SF32_PORT"

sftool \
  -c SF32LB52 \
  -p "$SF32_PORT" \
  -b 1000000 \
  --before default_reset \
  --after soft_reset \
  write_flash \
  cmake_out/lckfb_huangshan_pi/nuttx.bin@0x12010000
```

如果频繁超时或校验失败，重新插拔 USB 后使用兼容模式：

```bash
sftool \
  -c SF32LB52 \
  -p "$SF32_PORT" \
  --compat \
  write_flash \
  cmake_out/lckfb_huangshan_pi/nuttx.bin@0x12010000
```

禁止把 `nuttx.bin` 烧录到未核对的其他地址。

### 6.3 打开串口

串口参数：1,000,000 baud，8N1，无流控。

```bash
test -c "$SF32_PORT"
picocom \
  -b 1000000 \
  --noreset \
  --lower-rts \
  --lower-dtr \
  "$SF32_PORT"
```

不要使用普通 `screen` 或 `cu`；它们可能无法释放 RTS，导致 SF32LB52 一直保持复位。

成功启动的关键输出：

```text
SFBL
...
NuttShell (NSH)
nsh>
```

## 7. 官方基线验收

当前板级 [`rcS`](https://github.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/src/etc/init.d/rcS) 会在启动 3 秒后自动执行：

```text
vapp hap://app/com.application.lyra.demo &
```

该 QuickApp 会占用 LVGL、framebuffer 和输入设备，可能覆盖 `fb` 的结果，并与 `lvgldemo` 或后续 `ai_watch` 争用显示资源。进入 `nsh>` 后先等待 3 秒，执行 `ps`；如果仍有 `vapp`/Lyra demo 任务，记下数字 PID，执行 `kill -15 <数字 PID>` 停止它，再进行显示基线测试。

然后按顺序执行：

```text
uname -a
ls /dev
free
ps
date
adc -n 1
i2c dev -b 0 0x38 0x38
i2c dev -b 1 0x49 0x49
buttons 5
fb
lvgldemo widgets
ostest
```

| 命令 | 验收点 |
|---|---|
| `uname -a` | 板名包含 `lckfb_huangshan_pi` |
| `ls /dev` | 至少看到 `console`、`fb0`、`lcd0`、`input0`、`buttons`、`rtc0` |
| `free` | 8 MB OPI-PSRAM 已被纳入可用堆，数值应合理 |
| `ps` | 任务列表可正常读取 |
| `date` | RTC 接口可读 |
| `adc -n 1` | `/dev/adc0` 完成一次采样 |
| `i2c dev -b 0 0x38 0x38` | 仅在 `/dev/i2c0` 探测 FT6146 的 `0x38` |
| `i2c dev -b 1 0x49 0x49` | 仅在 `/dev/i2c1` 探测 AW32001 的 `0x49` |
| `buttons 5` | 按下 KEY1/KEY2 能观察到事件 |
| `fb` | AMOLED 显示嵌套矩形 |
| `lvgldemo widgets` | LVGL 界面正常，触摸可操作 |
| `ostest` | 内核回归无致命失败 |

这里故意只扫描两个已知地址。广泛扫描 I2C 地址可能对部分设备产生副作用。官方 README 中未指定 `-b 1` 的 AW32001 扫描会落到默认 `/dev/i2c0`；本手册已根据板级 [`sifli_ap.c`](https://github.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/src/sifli_ap.c) 的实际注册修正。

建议保存：

- 首次成功的 `repo manifest -r` 输出。
- 完整编译日志。
- 烧录日志。
- 串口启动与上述命令的输出。
- LCD 和触摸正常的照片或视频。

到这里才算“开发环境完成”。

## 8. 为项目建立独立配置

官方 `configs/nsh` 是基线，不建议直接把所有项目配置堆进去。基线验收后，创建项目变体：

```bash
cd /home/ma/openvela-sf32lb52

cp -a \
  vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/nsh \
  vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/ai_watch
```

此时**先不配置 `cmake_out/ai_watch`**。NuttX CMake 会在首次配置时扫描 `apps` 并生成 Kconfig；必须先完成第 9 章的最小 `ai_watch` 应用骨架，然后使用一个全新构建目录进行第一次 CMake 配置。该行为可在官方 [`nuttx_kconfig.cmake`](https://github.com/open-vela/nuttx/blob/dev-ai-contest-2026/cmake/nuttx_kconfig.cmake) 中核对。

另外，项目配置不能继续默认启动 Lyra QuickApp demo。如果项目采用原生 C + LVGL，让 AI 先检查 QuickApp/UIKit 配置依赖，然后：

- 停用项目不需要的 QuickApp/vapp 相关 CONFIG。
- 为 `rcS` 增加项目配置守护，或提供 `ai_watch` 专用启动脚本。
- 在启动策略完成前，从 NSH 手工运行 `ai_watch`，不让 Lyra demo 与它同时使用显示和输入。

规则：

- `cmake_out/ai_watch/.config` 是生成物，不直接交付。
- `ninja savedefconfig` 会把结果写回当前选中的 `configs/ai_watch/defconfig`，不再手工 `cp defconfig`。
- 直接编辑 defconfig 或 `repo sync` 更新它后，已存在的 CMake 树不会只因文件内容变化就自动重载；先执行 `ninja resetconfig`。
- 每次改 defconfig 都要配套编译和板端验收。
- 不要为了解决一个链接错误随意开启大量 CONFIG。

## 9. 建议的应用结构

原型阶段可以在 `apps/examples/ai_watch` 建立一个 NuttX 内建应用：

```text
apps/examples/ai_watch/
├── CMakeLists.txt
├── Kconfig
├── Makefile
├── ai_watch_main.c
├── app/
│   ├── app_controller.c
│   └── app_state.c
├── ui/
│   ├── screen_home.c
│   ├── screen_activity.c
│   ├── screen_reminder.c
│   └── ui_router.c
├── services/
│   ├── reminder_service.c
│   ├── motion_service.c
│   ├── ble_service.c
│   └── storage_service.c
├── platform/
│   ├── input_adapter.c
│   ├── display_adapter.c
│   ├── rtc_adapter.c
│   └── sensor_adapter.c
└── protocol/
    ├── phone_protocol.c
    └── phone_protocol.h
```

这是责任划分，不是要求第一天就创建全部文件。只在功能真正出现时增加相应模块。

代码边界：

| 代码类型 | 放置位置 |
|---|---|
| 手表页面、状态机、业务规则 | `apps/examples/ai_watch` |
| 对 `/dev/fb0`、`/dev/input0`、RTC、sensor 的业务适配 | `apps/examples/ai_watch/platform` |
| 黄山派引脚、上电、board bring-up | `vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi` |
| SF32LB52 芯片层 | `vendor/sifli/chips/sf32lb52` |
| 可复用的 NuttX 通用驱动 | `nuttx/drivers` |
| 功能开关 | `configs/ai_watch/defconfig` + 相应 `Kconfig` |
| 构建产物 | `cmake_out/ai_watch`，禁止手工编辑 |

`apps/examples/hello` 可作为 CMake/Kconfig/Makefile 最小模板；LVGL 相关用法优先参考 `apps/graphics/lvgl` 和 `apps/examples/lvgldemo`。

### 9.1 先建立最小骨架

第一次只创建：

```text
apps/examples/ai_watch/CMakeLists.txt
apps/examples/ai_watch/Kconfig
apps/examples/ai_watch/Makefile
apps/examples/ai_watch/ai_watch_main.c
```

配置符号统一使用 `CONFIG_EXAMPLES_AI_WATCH`，命令名使用 `ai_watch`。首版只打印版本和启动状态，不要立即创建全部 UI/service 文件。

检查骨架存在：

```bash
cd /home/ma/openvela-sf32lb52
test -f apps/examples/ai_watch/CMakeLists.txt
test -f apps/examples/ai_watch/Kconfig
test -f apps/examples/ai_watch/Makefile
test -f apps/examples/ai_watch/ai_watch_main.c
```

### 9.2 应用存在后才进行首次 CMake 配置

如果在创建 `apps/examples/ai_watch` 之前已经配置过 `cmake_out/ai_watch`，不要复用它。先改名保留：

```bash
cd /home/ma/openvela-sf32lb52
test -d cmake_out/ai_watch
test ! -e cmake_out/ai_watch.before-app
mv -T cmake_out/ai_watch cmake_out/ai_watch.before-app
```

然后配置全新的项目构建树：

```bash
cd /home/ma/openvela-sf32lb52
export PATH=/home/ma/.local/bin:$PATH

cmake \
  -B cmake_out/ai_watch \
  -S "$PWD/nuttx" \
  -GNinja \
  -DBOARD_CONFIG=../vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/ai_watch \
  -DEXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=return-mismatch -D_LIBCPP_WORKAROUND_OBJCXX_COMPILER_INTRINSICS"
```

在自己的交互终端中打开 menuconfig，搜索 `EXAMPLES_AI_WATCH` 并启用：

```bash
cd /home/ma/openvela-sf32lb52/cmake_out/ai_watch
ninja menuconfig
ninja savedefconfig
ninja
```

不要在 `ninja savedefconfig` 后手工复制构建目录的 `defconfig`；当前 CMake target 已经会更新源配置。

### 9.3 何时使用 `resetconfig`

在下列情况后，先重新从源 defconfig 生成 `.config`：

- 直接编辑了 `configs/ai_watch/defconfig`。
- `repo sync` 更新了 defconfig。
- 切换 commit 后 defconfig 变化。

```bash
cd /home/ma/openvela-sf32lb52/cmake_out/ai_watch
ninja resetconfig
ninja
```

如果是在已配置后才新增一个完整的 apps 子目录，`resetconfig` 不足以保证重新生成 apps Kconfig；应改用一个全新的 CMake 构建目录。

## 10. AI coding 的固定闭环

### 10.1 每个功能都按这个顺序

1. 向 AI 给出本次唯一目标和验收方法。
2. AI 先读取板级 README、defconfig、相关 Kconfig/CMake 和现有例程。
3. AI 列出要修改的仓库、文件和 CONFIG，确认没有跨越代码边界。
4. AI 仅实现本次功能。
5. AI 执行格式/静态检查和完整编译。
6. 开发板连接后，AI 或开发者烧录固件。
7. 通过 NSH 命令、串口日志和界面操作验收。
8. 把失败日志原样给 AI，一次只修一类问题。
9. 执行 `git diff --check` 和最后一次回归。
10. 每个可用功能对应一个独立 commit。

### 10.2 给 AI 的通用任务模板

```text
目标板：LCKFB Huangshan Pi / SF32LB52
源码：/home/ma/openvela-sf32lb52
分支基线：dev-ai-contest-2026
项目配置：vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/ai_watch

本次唯一目标：<填写一个功能>
板端验收：<填写一条可观察的成功条件>

开始前必须读取：
1. vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md
2. vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi/configs/ai_watch/defconfig
3. 相关的板级 CMakeLists.txt、Kconfig、src 和引脚定义
4. apps 中最接近的可编译例程

约束：
- 不使用 RT-Thread API。
- 不整段复制 SiFli SDK/RT-Thread 工程。
- 不修改 cmake_out 生成物。
- 不手工修改生成的 .config 作为最终方案。
- 业务代码放 apps，板级初始化才放 vendor/sifli。
- 只做与本次目标相关的最小改动。
- 不要覆盖工作区中的其他未提交修改。

交付要求：
- 先说明依赖、设备节点、修改文件和风险。
- 实现代码及所需 Kconfig/CMake/defconfig。
- 执行完整 CMake/Ninja 编译。
- 给出烧录后的 NSH 验收命令。
- 报告已验证内容和未验证风险。
```

### 10.3 串口日志驱动的修复模板

```text
下面是同一版固件的完整构建日志和串口日志。
请先区分是配置、编译、链接、驱动初始化还是业务逻辑问题。
只修复最早出现的根因，不要同时重构其他模块。
修复后重新执行原编译命令，并列出板端回归项。
```

## 11. 分阶段开发路线

### M0：官方基线

目标：证明源码、工具链、烧录和硬件可用。

交付：

- `nuttx.bin`
- manifest 锁定文件
- 启动日志
- `free/ps/fb/lvgldemo/ostest` 验收记录

该阶段不写项目业务代码。

### M1：最小手表应用

目标：在 NSH 中运行 `ai_watch`，显示一个稳定首页。

功能：

- 显示时间、日期和蓝牙状态占位。
- LVGL 单页，先不做复杂动画。
- 按键可退出或切换一个简单状态。
- 运行 30 分钟不崩溃、不明显泄漏。

验收：

```text
nsh> ai_watch
```

屏幕显示首页，按键有反馈，串口无 assertion/fault。

### M2：触摸和页面导航

目标：建立稳定的输入与页面路由，再增加功能页。

功能：

- FT6146 点按/滑动事件。
- KEY1/KEY2 映射到统一输入事件。
- 首页、运动页、提醒页、设置页。
- 页面切换不重复创建大型资源。

验收：连续导航 100 次，没有错乱、卡死或持续内存下降。

### M3：RTC、计时器、提醒和存储

目标：在没有手机的情况下也能使用基本工具。

功能：

- RTC 时间读取和设置。
- 倒计时、秒表、闹钟/事项。
- 提醒数据存入 KVDB/LittleFS 或已验证的 NOR 存储方案。
- 异常断电后数据仍可恢复。

验收：新增 10 条提醒，重启后仍存在，且到点能触发界面和声音/震动中已实现的部分。

### M4：运动与跌倒检测

当前官方 README 明确说明 `/dev/uorb` 暂为空，sensor uORB 集成在后续 PR。因此此阶段必须先确认当前分支的 LSM6DSL 实际接口，不能假设 `bmi160` 例程可直接使用。

顺序：

1. 运行已启用的 `lsm6dsl_reader` 或检查实际 sensor 设备节点。
2. 连续记录静止、走路、跑步、抬腕和受控倒伏数据。
3. 先做离线阈值分析，再把算法放到板端。
4. 跌倒判定至少组合加速度幅值、姿态变化、冲击后静止时间和用户取消倒计时。
5. 在样本不足时，只报告“疑似跌倒”，不直接宣称医疗级可靠性。

验收：记录测试集、阈值、误报/漏报，并能在事件触发后用按键或触摸取消告警。

### M5：BLE 与手机协议

基线 defconfig 已开启多项 Bluetooth/H4/GATT 配置，但“CONFIG 已开启”不等于手表业务链路已验证。

先完成：

- 广播、连接、断开和自动重连。
- 单一自定义 GATT service。
- 协议包含 `version`、`type`、`request_id`、`timestamp`、`payload`。
- 手机向手表下发时间、通知、日程和 AI 结果。
- 手表向手机上报按键事件、状态、运动数据和疑似跌倒事件。
- 为消息设置最大长度、分包、超时、重试、去重和 ACK。

验收：连续往返 1000 条小消息，主动断开后可恢复，重复包不重复执行。

### M6：语音和 AI

首版采用手机协同：

```text
手表事件/语音入口
        -> BLE
手机 App
        -> 语音识别 / 网络 / AI
结构化结果
        -> BLE
手表显示 / 提醒 / 播放
```

建议顺序：

1. 先用手机麦克风完成 ASR/AI 闭环。
2. AI 返回结构化指令，例如 `show_text`、`create_reminder`、`start_timer`、`notify_user`，不让手表解析自由文本命令。
3. 再验证板载麦克风、音频驱动、录音格式和 BLE 带宽。
4. 最后接入扬声器/TTS。

音频不在官方黄山派 `nsh` README 的已验证外设列表中，所以不要在 M0-M2 承诺完整语音链路。

### M7：功耗、稳定性和比赛交付

- 屏幕亮度和息屏策略。
- BLE 连接参数和重连策略。
- 传感器采样率分级。
- 闲时休眠与 RTC/按键/触摸唤醒。
- 长时间运行、反复连接、存储写入和异常断电测试。
- 记录版本 manifest、固件 hash、固件尺寸、已知问题和演示脚本。

## 12. Git 和多仓库管理

Repo 工作树的顶层不是一个 Git 仓库。`apps`、`nuttx`、`vendor/sifli` 是独立仓库，需要分别创建分支和提交。

### 12.1 警告：目录 ≠ 仓库，存在 200+ 嵌套仓库

这个 manifest 把大量第三方代码作为**独立仓库**嵌套检出。例如：

| 你以为的路径 | 实际归属 |
|---|---|
| `apps/frameworks/` | 符号链接 → `frameworks/` 仓库 |
| `frameworks/runtimes/feature/` | 独立仓库 `frameworks_runtimes_feature` |
| `apps/graphics/lvgl/`、`apps/system/libuv/` 等 | 各自是独立仓库（`apps_graphics_lvgl`…） |
| `nuttx/libs/libxx/libcxx/` | 独立仓库 `nuttx_libs_libxx_libcxx` |

**因此**：

- 不要用 `git -C apps status` 判断某个文件的修改状态——它可能根本不属于 apps。
- 改任何代码前，先确认归属：`repo status | grep <路径片段>` 或在文件所在目录直接跑 `git status`。
- 提交要在**实际所属仓库**里建分支、提交。已完成的实例：`feature_utils.h` 修复提交在 `frameworks/runtimes/feature` 仓库的 `contest/ai-watch` 分支（commit `a2afa86`），而不是 apps。
- `frameworks/.gitignore` 用 `/*/` 忽略所有一级子目录，这是刻意设计（那些目录全是嵌套仓库）。

查看总体状态：

```bash
cd /home/ma/openvela-sf32lb52
repo status
```

只在需要修改的仓库创建工作分支：

```bash
git -C apps switch -c contest/ai-watch
git -C vendor/sifli switch -c contest/ai-watch
```

如果没有修改 `vendor/sifli`，就不需要在该仓库创建分支。只有新增通用 NuttX 驱动时才修改 `nuttx`。

提交前：

```bash
git -C apps status --short
git -C apps diff --check
git -C apps diff

git -C vendor/sifli status --short
git -C vendor/sifli diff --check
git -C vendor/sifli diff

cmake --build /home/ma/openvela-sf32lb52/cmake_out/ai_watch --parallel 4
```

不要把不同仓库的改动误以为能在顶层通过一次 `git commit` 提交。

## 13. 已知限制和常见问题

### 13.1 分支错误

症状：SF32LB52 芯片目录、Kconfig 或板级依赖缺失。

处理：确认 manifest 是 `dev-ai-contest-2026`，不要切换到 `dev` 或 `trunk`。官方板级 README 明确说明后两者目前缺少芯片层依赖。

### 13.2 路径含中文或空格

症状：CMake、Kconfig、脚本或 IDE 出现难以解释的路径错误。

处理：源码只放在 `/home/ma/openvela-sf32lb52`，不要放在 `/home/ma/桌面/work`。

### 13.3 Git LFS 文件只有几百字节

症状：预编译库是包含 `version https://git-lfs.github.com/spec/v1` 的文本指针。

处理：

```bash
git lfs install
cd /home/ma/openvela-sf32lb52
repo sync -c -j2
```

然后在报错的具体子仓库中执行 `git lfs pull`。

### 13.4 缺少 Kconfiglib、Ninja 或 ARM GCC

症状：CMake 报找不到 `olddefconfig`，或者出现 `ninja: command not found`、找不到 `arm-none-eabi-gcc`。

处理：

```bash
sudo apt install -y ninja-build genromfs xxd
python3 -m pip install --user kconfiglib pyelftools cxxfilt devicetree
export PATH="$HOME/opt/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin:$PATH"
export PATH=/home/ma/.local/bin:$PATH

command -v olddefconfig
command -v savedefconfig
command -v ninja
arm-none-eabi-gcc --version | head -1   # 必须是 14.2.1，见 5.1.1
```

### 13.5 `sftool` 无法连接

按顺序检查：

1. 用户是否属于 `dialout`。
2. `SF32_PORT` 是否仍指向上电前核对过的 CH340N。
3. 是否有其他串口程序占用设备。
4. 重新插拔 USB，在 ROM bootloader 约 2 秒监听窗口内重试。
5. 尝试 `--compat`。
6. 更换稳定供电和 USB 线。

### 13.6 串口打开后板子不运行

原因通常是串口软件把 RTS 拉低。使用：

```bash
test -c "$SF32_PORT"
picocom -b 1000000 --noreset --lower-rts --lower-dtr "$SF32_PORT"
```

### 13.7 LCD 打印 ID 不匹配

仅 USB 供电时可能看到：

```text
[co5300] ReadID=0x0 expected 0x331100, init anyway
```

这是官方 README 记录的已知现象。先检查屏幕是否实际可显示，再决定是否需要调试供电或 QSPI。

### 13.8 传感器/uORB

- 当前官方 README 记录 `/dev/uorb` 为空。
- `CONFIG_SENSORS_LSM6DSL=y` 和 `CONFIG_EXAMPLES_LSM6DSL_READER=y` 已在基线 defconfig 中，但开发前仍要以当前同步的代码和实际设备节点为准。
- `apps/examples/bmi160` 只能参考上层采样模式，不是板载 LSM6DSL 的直接解决方案。

### 13.9 存储和音频

- 官方 README 说明该板没有引出 SD 卡，不要把 TF/SD 作为 openvela 基线功能。
- AW32001 当前只以裸 I2C 形式暴露，完整充电服务需要后续实现和验证。
- 音频不是当前板级 README 列出的已验证基线，应在 UI、传感器和 BLE 之后单独 bring-up。

### 13.10 RT-Thread/SiFli SDK 例程的使用边界

`LCHSP_Watch` 和 SiFli SDK 例程可以参考：

- 硬件引脚、上电时序和传感器地址。
- 手表界面信息架构。
- BLE 业务流程。
- 功能拆分和交互逻辑。

不能直接复制：

- RT-Thread device API、线程 API 和初始化注册宏。
- SiFli SDK SCons 工程文件。
- LVGL 8 与当前 openvela LVGL 版本不兼容的接口。
- 与 NuttX upper-half/lower-half 驱动模型冲突的驱动代码。

### 13.11 `buttons` 例程守护进程异常（2026-08-23 实测）

症状：`buttons` 启动后第一轮 `poll` 返回假事件（`poll returned: 1`），随后守护进程卡在 `Ready` 状态不再运行、不打印任何事件；按键无反应。**该僵尸任务还会导致 `ostest` 卡死在 `wqueue_test: LPWORK`。**

处理：`kill -15 <button_daemon 的 PID>` 后 ostest 可完整跑完（status=0）。

结论：KEY2（GPIO1_43，高电平有效，仅此一键注册）的硬件验证推迟到 M1——`ai_watch` 用自研输入路径直接 read `/dev/buttons` 再验。触摸（FT6146@0x38 i2c0 应答 + 实际点击正常）不受影响。

### 13.12 `fb` 例程只显示首帧（不是屏幕故障）

现象：跑完 `fb` 屏幕呈全屏淡粉色。

解释：那正是例程画的第一层矩形——RGB16_VIOLET=`0xEC1D`≈(236,168,242) 淡粉紫、全屏大小。后续嵌套矩形未被刷新到面板。LVGL 连续渲染路径完全正常（Lyra demo 界面+触摸实测 OK）。不要据此怀疑屏幕硬件。

### 13.13 picocom 回车无效：必须加 `--omap crlf`

症状：串口里能打字能删除，但按回车命令不执行（Ctrl+J 有效）。

处理：

```bash
picocom --omap crlf -b 1000000 --noreset --lower-dtr --lower-rts "$SF32_PORT"
```

建议同时带 `--logfile ~/huangshan-logs/xxx.log` 留档。退出是 Ctrl+A 然后 Ctrl+X。

### 13.14 Ubuntu 22.04 的 brltty 抢占 CH340

症状：USB 插上板子后 `lsusb` 能看到 `1a86:7523 QinHeng CH340`，但没有生成 `/dev/ttyUSB*`。

原因：brltty（盲文终端服务）的 udev 规则抢占 CH340。

处理：

```bash
sudo apt remove -y brltty
```

然后重新插拔 USB。

### 13.15 每次上电 Lyra demo 自动抢屏

rcS 在启动约 3 秒后拉起 `vapp hap://app/com.application.lyra.demo`（JS 应用，加载期间屏幕黑）。它占用 LVGL/显示/触摸，且运行过后 `lvgldemo` 会报 "LVGL already initialized" 无法再跑。

当前策略：进 NSH 后 `ps` 找到 vapp（通常 PID 8）→ `kill -15 8`。彻底停用留到第 3 天项目化启动时处理。

## 14. 第一周的建议执行清单

### 第 1 天：环境和源码

- [ ] 安装 Kconfiglib CLI、Ninja、ARM GCC、`genromfs`、`xxd`、Picocom、`sftool`。
- [ ] 加入 `dialout` 并重新登录。
- [ ] 初始化 `/home/ma/openvela-sf32lb52`。
- [ ] `repo sync -c -j4`。
- [ ] SF32LB52 libs 完成 `git lfs fsck` 和 archive 校验。
- [ ] 导出 `manifest-lock.xml`。

### 第 2 天：官方基线

- [ ] 不改代码完成官方 `nsh` 编译。
- [ ] 烧录 `nuttx.bin@0x12010000`。
- [ ] 进入 NSH。
- [ ] 停止默认 Lyra `vapp` 后再测试 framebuffer/LVGL。
- [ ] 完成全部基线命令。
- [ ] 保存日志。

### 第 3 天：项目骨架

- [ ] 创建 `configs/ai_watch`。
- [ ] 创建 `apps/examples/ai_watch` 最小应用。
- [ ] 应用文件存在后，用全新 `cmake_out/ai_watch` 首次配置。
- [ ] 启用 `CONFIG_EXAMPLES_AI_WATCH`并执行 `ninja savedefconfig`。
- [ ] 确定 QuickApp/vapp 停用或项目化启动策略。
- [ ] `nsh> ai_watch` 能打印版本和启动状态。
- [ ] 完成第一个独立 commit。

### 第 4-5 天：首页和输入

- [ ] LVGL 静态首页。
- [ ] RTC 时间显示。
- [ ] KEY1/KEY2 事件。
- [ ] FT6146 点按。
- [ ] 连续运行 30 分钟。

### 第 6-7 天：首个可演示闭环

- [ ] 首页、提醒页两页导航。
- [ ] 创建一个倒计时。
- [ ] 到时在屏幕上提醒。
- [ ] 重启、快速点击、页面反复切换测试。
- [ ] 录制第一个实机演示。

## 15. 官方文档和参考例程

### 优先级 A：必读官方资料

- [openvela 黄山派板级 README](https://github.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/README_zh-cn.md)
- [openvela `dev-ai-contest-2026` manifest](https://github.com/open-vela/manifests/blob/dev-ai-contest-2026/openvela.xml)
- [立创·黄山派 Wiki](https://wiki.lckfb.com/zh-hans/hspi-sf32lb52/)
- [黄山派硬件资料](https://wiki.lckfb.com/zh-hans/hspi-sf32lb52/hardware/board.html)
- [SiFli SF32LB52x SDK 文档](https://docs.sifli.com/projects/sdk/latest/sf32lb52x/index.html)
- [OpenSiFli/sftool Releases](https://github.com/OpenSiFli/sftool/releases)

### 优先级 B：同步后阅读的 openvela 本地资料

```text
nuttx/Documentation/quickstart/compiling_cmake.rst
nuttx/Documentation/quickstart/configuring.rst
nuttx/Documentation/guides/customapps.rst
nuttx/Documentation/guides/customboards.rst
apps/examples/hello
apps/examples/buttons
apps/examples/ble
apps/examples/lsm6dsl_reader
apps/examples/lvgldemo
apps/graphics/lvgl
vendor/sifli/boards/sf32lb52/lckfb_huangshan_pi
```

### 优先级 C：只作硬件/功能参考

- [Gangan-307/LCHSP_Watch](https://github.com/Gangan-307/LCHSP_Watch)：SiFli SDK + RT-Thread + LVGL 8 完整手表参考。
- [OpenSiFli/lckfb-hspi-ulp_example](https://github.com/OpenSiFli/lckfb-hspi-ulp_example)：黄山派厂商外设、sensor 和 LVGL 例程。
- [OpenSiFli/SiFli-SDK](https://github.com/OpenSiFli/SiFli-SDK)：HAL、上电时序和硬件参考。

这些是 RT-Thread/SiFli SDK 资料，不是 openvela/NuttX 应用模板。

## 16. 开始写项目代码的门槛

以下项目全部打勾后，再让 AI 开始创建 `ai_watch`。（状态截至 2026-08-23 晚）

- [x] 源码在 `/home/ma/openvela-sf32lb52`。
- [x] manifest 是 `dev-ai-contest-2026`。
- [x] `vendor/sifli` 和 SF32LB52 libs 已同步，LFS 和 `.a` archive 校验通过。
- [x] `olddefconfig`、`savedefconfig`、Ninja、ARM GCC、`genromfs`、`xxd` 均可在 `PATH` 中找到（ARM GCC 用 ~/opt 的 14.2，见 3.2.1）。
- [x] 官方 `nsh` 编译成功（含 5.1 节记录的 3 处偏差：工具链 14.2、EXTRA_FLAGS 扩展、feature_utils.h 一行修复）。
- [x] `nuttx.bin` 已在 `0x12010000` 启动。
- [x] NSH 可用（注：`uname -a` 不含板名，此基线即如此；以 `ls /dev` 设备集和启动日志为准）。
- [x] LCD、LVGL、触摸完成基线测试（按键见下条）。
- [ ] 按键基线测试——**受阻于 13.11 例程 bug**，M1 用自研输入路径补验后勾选。
- [x] 已导出 `manifest-lock.xml`（并建议备份到 `~/huangshan-logs/`）。
- [x] 旧 simulator 源码树已删除，不再作为开发依赖。
- [ ] 已创建 `configs/ai_watch`，官方 `configs/nsh` 保持不变（M1 第一步）。
- [x] 已记录默认 Lyra `vapp` 与项目 LVGL 的冲突及处理策略（见 13.15：每次上电 `kill -15 8`，彻底停用留到项目化启动）。

达到这个门槛后，后续每个 AI coding 任务都能在真实、可编译、可烧录的基线上进行，不会把环境问题误判为代码问题。
