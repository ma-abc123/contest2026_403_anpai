#!/usr/bin/env python3
"""Live monitor for a fall-dataset capture run.

Reads the console of a watch running the capture build
(4.8.63-capture, AI_WATCH_CAPTURE_STREAM=1) and:

  * writes every raw serial line to the log file (no prefixes, so the
    file is directly usable by tools/capture_analyze.py and
    tools/fall_threshold_analysis.py);
  * prints one status line per second: stream rate, sample count, and
    which scripted action you should be doing right now;
  * prints detections immediately, so you know whether an action
    triggered without looking at the watch.

Usage:
    python3 tools/capture_monitor.py --log ~/huangshan-logs/cap_run1.log

Stop with Ctrl-C; a summary is printed at the end.

The action script below matches docs/development/采集操作卡-2026-11-03.md.
Timing assumes the capture build's compressed quiet windows (3 s countdown
+ 3 s cooldown), so 8 s per action is enough for the state machine to
re-arm between falls.
"""

import argparse
import os
import re
import sys
import time

import serial

REC_RE = re.compile(r"REC,(\d+),")

# (label, expect_detect). expect_detect=None means "setup / idle".
ACTIONS = [
    ("静置（等 boot 与 BLE recovery 平息）", None),
    ("拿起手表 → 戴上", False),
    ("站立 → 侧倒在床垫", True),
    ("站立 → 前倒在床垫", True),
    ("快速坐下（椅子）", False),
    ("重重坐下（屁股砸椅子）", False),
    ("用手拍打手表 3 下", False),
    ("原地跳 2 下", False),
    ("正常走路 5 秒", False),
    ("脱下手表放到桌上", False),
    ("从桌上拿起 → 戴上", False),
    ("站立 → 后倒在床垫", True),
    ("拿起放下 ×3（误报复现）", False),
]

IDLE_S = 25.0          # idle window before action #1
SLOT_S = 8.0           # one action + 3 s gap

C_RED = "\033[31m"
C_YEL = "\033[33m"
C_GRN = "\033[32m"
C_DIM = "\033[2m"
C_OFF = "\033[0m"


def action_at(elapsed):
    """Return (index, label, expect, seconds_into_slot, remaining)."""

    if elapsed < IDLE_S:
        return (0, ACTIONS[0][0], None, elapsed, IDLE_S - elapsed)

    slot = (elapsed - IDLE_S) // SLOT_S
    idx = int(slot) + 1
    if idx >= len(ACTIONS):
        return (None, None, None, 0.0, 0.0)

    into = (elapsed - IDLE_S) % SLOT_S
    return (idx, ACTIONS[idx][0], ACTIONS[idx][1], into, SLOT_S - into)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--log", default=os.path.expanduser(
        "~/huangshan-logs/cap_run1.log"))
    ap.add_argument("--no-script", action="store_true",
                    help="不要动作提示，只显示流状态")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05)
    except serial.SerialException as exc:
        print(f"打不开 {args.port}: {exc}")
        print("先停投屏守护: systemctl --user stop ai-watch-viewer")
        return 1

    # The board's reset circuit is wired to RTS: leaving it asserted holds
    # the SoC in reset (black screen, silent UART).
    ser.dtr = False
    ser.rts = False

    os.makedirs(os.path.dirname(os.path.abspath(args.log)), exist_ok=True)
    log = open(args.log, "w", encoding="utf-8", errors="replace")

    print(f"日志 → {args.log}")
    print("Ctrl-C 结束。开始 25 秒静置计时，之后每 8 秒一个动作。\n")

    t0 = time.time()
    t_next_status = t0
    buf = b""
    rec_count = 0
    last_rec_ms = 0
    rec_times = []          # wall clock of recent REC lines (rate)
    detections = []         # (elapsed_s, text)
    gaps = []

    try:
        while True:
            data = ser.read(4096)
            now = time.time()

            if data:
                text = data.decode("utf-8", "replace")
                log.write(text)
                log.flush()
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").strip()

                    m = REC_RE.search(line)
                    if m:
                        t_ms = int(m.group(1))
                        if last_rec_ms and t_ms > last_rec_ms:
                            gap = t_ms - last_rec_ms
                            if gap > 200:
                                gaps.append((t_ms, gap))
                        last_rec_ms = t_ms
                        rec_count += 1
                        rec_times.append(now)
                        if len(rec_times) > 50:
                            rec_times.pop(0)
                        continue

                    if "MOTION: fall armed" in line:
                        el = now - t0
                        print(f"\n{C_RED}>>> [{el:6.1f}s] 触发告警  {line}"
                              f"{C_OFF}")
                        detections.append((el, "ARMED"))
                    elif "MOTION: impact but orientation kept" in line:
                        el = now - t0
                        print(f"\n{C_YEL}>>> [{el:6.1f}s] 冲击但姿态未变  "
                              f"{line}{C_OFF}")
                        detections.append((el, "REJECT"))
                    elif "MOTION: TEST fall" in line:
                        el = now - t0
                        print(f"\n{C_DIM}>>> [{el:6.1f}s] {line}{C_OFF}")

            # ---- once-a-second status ----
            if now >= t_next_status:
                t_next_status = now + 1.0
                elapsed = now - t0
                rate = 0.0
                if len(rec_times) >= 2:
                    span = rec_times[-1] - rec_times[0]
                    if span > 0:
                        rate = (len(rec_times) - 1) / span

                if args.no_script:
                    print(f"[{elapsed:6.1f}s] {rate:5.1f}Hz "
                          f"rec={rec_count}", end="\r", flush=True)
                else:
                    idx, label, expect, into, left = action_at(elapsed)
                    if idx is None:
                        print(f"\n动作表已走完（{elapsed:.0f}s）。"
                              f"Ctrl-C 结束，日志在 {args.log}")
                        args.no_script = True
                        continue

                    if expect is None:
                        hint = f"静置中（{left:.0f}s 后开始）"
                    else:
                        mark = "应检出" if expect else "不应检出"
                        hint = f"#{idx} {label}（{mark}，剩 {left:.0f}s）"

                    print(f"[{elapsed:6.1f}s] {rate:5.1f}Hz rec={rec_count:5d}"
                          f"  gap={len(gaps):d}  {hint}")

    except KeyboardInterrupt:
        pass
    finally:
        log.close()
        ser.close()

    print("\n\n=== 采集摘要 ===")
    print(f"时长 {time.time() - t0:.0f}s   REC 样本 {rec_count}")
    print(f"检出 {len(detections)} 次：")
    for (el, kind) in detections:
        print(f"  {el:6.1f}s  {kind}")
    if gaps:
        print(f"断流 {len(gaps)} 处（>200ms）：")
        for (t, g) in gaps[:10]:
            print(f"  t={t / 1000.0:.1f}s  gap={g}ms")
    else:
        print("无断流，采样连续。")

    return 0


if __name__ == "__main__":
    sys.exit(main())
