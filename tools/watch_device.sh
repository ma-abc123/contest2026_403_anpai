#!/bin/bash
# Auto-open the watch preview whenever the board is plugged in.
#
# The panel and its touch layer are dead, so this window IS the watch
# interface. Opening it by hand after every replug gets old fast.
#
# Polls /dev/ttyUSB0 (udev rules need root on this box), starts the
# viewer when the port appears, and stops it when the port goes away.
# Safe to run repeatedly: it tracks the child by pid and never starts a
# second copy.
#
#   tools/watch_device.sh &          # foreground-ish, logs to stdout
#   nohup tools/watch_device.sh &    # detached

set -u

PORT="${WATCH_PORT:-/dev/ttyUSB0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VIEWER="$HERE/shot_remote.py"
LOG="${WATCH_LOG:-/tmp/shot_remote.log}"
PIDFILE="/tmp/aiwatch_viewer.pid"
PY="${WATCH_PYTHON:-/usr/bin/python3}"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

viewer_alive() {
    [ -f "$PIDFILE" ] || return 1
    local pid
    pid=$(cat "$PIDFILE" 2>/dev/null) || return 1
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

log "守护中：等待 $PORT 出现即自动打开预览"

while true; do
    if [ -e "$PORT" ]; then
        if ! viewer_alive; then
            # 清掉可能残留的旧进程，避免两个程序抢串口
            pkill -f "shot_remote.py -p $PORT" 2>/dev/null
            sleep 1

            DISPLAY="${DISPLAY:-:0}" "$PY" -u "$VIEWER" -p "$PORT" \
                > "$LOG" 2>&1 &
            echo $! > "$PIDFILE"
            log "设备已插入，预览已启动 (pid $(cat "$PIDFILE"))"
        fi
    else
        if viewer_alive; then
            kill "$(cat "$PIDFILE")" 2>/dev/null
            rm -f "$PIDFILE"
            log "设备已移除，预览已停止"
        fi
    fi

    sleep 1
done
