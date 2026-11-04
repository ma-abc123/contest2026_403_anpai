#!/bin/bash
# 一键采集：停投屏 → 打开动作提示窗口 → 结束后自动恢复投屏。
#
# 用法：
#   tools/run_capture.sh                 # 日志 → ~/huangshan-logs/cap_run1.log
#   tools/run_capture.sh /tmp/run2.log   # 指定日志路径
#
# 窗口里按 空格 开始计时（先静置 25 秒），F 全屏，Esc 退出。
# 串口原始日志同时写入上面的文件，可直接交给 tools/capture_analyze.py。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="${1:-$HOME/huangshan-logs/cap_run1.log}"

# 投屏守护独占 /dev/ttyUSB0，采集前必须让出来。
systemctl --user stop ai-watch-viewer 2>/dev/null || true
restore() { systemctl --user start ai-watch-viewer 2>/dev/null || true; }
trap restore EXIT INT TERM

echo "日志 → $LOG"
# 固定用系统 Python：PyGObject(gi) 只装在 /usr/lib/python3/dist-packages。
/usr/bin/python3 "$ROOT/tools/capture_display.py" --log "$LOG"
