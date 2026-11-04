#!/bin/bash
# 编译真屏（面板）固件：AI_WATCH_HEADLESS 置 0。
#
# 屏幕是坏的，平时跑的是 headless 版（影子帧 + 主机远端指针）。新屏装上
# 那天要换成正屏版：flush 走 putarea()、启用 FT6146 触摸、页面切 200ms 淡入。
# 两个版本除这个宏外完全相同，切换只靠重编 + 重烧。
#
# 临时改宏、编译完无论成功失败都恢复成 1，保证工作区始终是 headless 状态。
#
# 用法：
#   tools/build_panel.sh                      # 输出到 ~/huangshan-logs/bin_panel.bin
#   tools/build_panel.sh /tmp/panel.bin
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/contest2026_403_anpai/app/ai_watch/ai_watch_main.c"
OUT="${1:-$HOME/huangshan-logs/bin_panel.bin}"

if [ ! -f "$SRC" ]; then
    echo "找不到 $SRC" >&2
    exit 1
fi

cp "$SRC" "$SRC.panelbak"
restore() {
    mv -f "$SRC.panelbak" "$SRC"
    # 必须删依赖目标：本盘禁 utimensat，touch 改不动 mtime，make 会把
    # 按 HEADLESS=0 编出来的 .o 当成仍然有效，于是"恢复成 headless"的
    # 构建里其实跑的是面板版（2026-11-03 在采集宏上踩过同一个坑）。
    find "$ROOT/cmake_out" -name 'ai_watch_main.c.o' -delete 2>/dev/null || true
}
trap restore EXIT

python3 - "$SRC" <<'PY'
import sys

p = sys.argv[1]
s = open(p).read()
old = '#define AI_WATCH_HEADLESS           1'
new = '#define AI_WATCH_HEADLESS           0'
if old not in s:
    raise SystemExit(f'宏不是预期的 1，先手工检查 {p}')
open(p, 'w').write(s.replace(old, new, 1))
print('AI_WATCH_HEADLESS -> 0 (panel mode)')
PY

# /home/ma/bin 里有 touch 包装器（容器禁 utimensat）
export PATH="/home/ma/bin:$ROOT/prebuilts/tools/linux/x86_64:/usr/bin:$PATH"
export TMPDIR=/home/ma/tmp

cmake --build "$ROOT/cmake_out/prod" -j 12

cp "$ROOT/cmake_out/prod/nuttx.bin" "$OUT"
echo "面板固件 → $OUT"
echo "烧录：systemctl --user stop ai-watch-viewer"
echo "      sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 --before default_reset \\"
echo "        --after soft_reset write_flash $OUT@0x12010000"
