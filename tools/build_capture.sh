#!/bin/bash
# 编译采集固件（4.8.63-capture）。
#
# 临时把 ai_watch_motion.h 里的 AI_WATCH_CAPTURE_STREAM 置 1，编译完
# 无论成功失败都恢复成 0，保证工作区始终是"正常固件"状态。
#
# 用法：
#   tools/build_capture.sh                    # 输出到 ~/huangshan-logs/bin_4863_capture.bin
#   tools/build_capture.sh /tmp/my_capture.bin
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HDR="$ROOT/contest2026_403_anpai/app/ai_watch/ai_watch_motion.h"
OUT="${1:-$HOME/huangshan-logs/bin_4863_capture.bin}"

if [ ! -f "$HDR" ]; then
    echo "找不到 $HDR" >&2
    exit 1
fi

cp "$HDR" "$HDR.capbak"
restore() { mv -f "$HDR.capbak" "$HDR"; }
trap restore EXIT

python3 - "$HDR" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '#define AI_WATCH_CAPTURE_STREAM     0'
new = '#define AI_WATCH_CAPTURE_STREAM     1'
if old not in s:
    raise SystemExit(f'宏不是预期的 0，先手工检查 {p}')
open(p, 'w').write(s.replace(old, new, 1))
print('AI_WATCH_CAPTURE_STREAM -> 1')
PY

# /home/ma/bin 里有 touch 包装器（容器禁 utimensat）
export PATH="/home/ma/bin:$ROOT/prebuilts/tools/linux/x86_64:/usr/bin:$PATH"
export TMPDIR=/home/ma/tmp

cmake --build "$ROOT/cmake_out/prod" -j 12

cp "$ROOT/cmake_out/prod/nuttx.bin" "$OUT"
echo "采集固件 → $OUT"
echo "烧录：sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 --before default_reset \\"
echo "        --after soft_reset write_flash $OUT@0x12010000"
