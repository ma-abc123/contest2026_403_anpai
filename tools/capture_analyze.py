#!/usr/bin/env python3
"""Offline analysis of a fall-dataset capture run.

Consumes the raw serial log written by tools/capture_monitor.py (or any
console capture of the capture build), replays it through the *same*
state machine the firmware runs, and prints a per-action feature table:

    #  action                    samples  |a|min  |a|max  ff_ms  angle  z/xy  fired  expect  result

The feature columns are the ones that separate a real fall from the
"pick it up, wiggle it, put it down" false positive:

    ff_ms    longest continuous free-fall run (|a| < 550 mg).  A real fall
             holds the low-g state for 150-400 ms; a hand movement breaks
             it into fragments shorter than 100 ms.
    |a|max   impact peak.  Falls land at 2400-4000 mg, a table top rarely
             exceeds 2000.
    angle    gravity direction change between the start and the end of the
             slot.
    z/xy     terminal gravity direction: lying on its side gives < 1,
             face-up/face-down on a table gives a large value.

Usage:
    python3 tools/capture_analyze.py ~/huangshan-logs/cap_run1.log
    python3 tools/capture_analyze.py cap.log --idle 25 --slot 8
    python3 tools/capture_analyze.py cap.log --scan 8     # unlabelled scan

Timing defaults match the capture build (3 s countdown + 3 s cooldown);
pass --cooldown 60000 --countdown 15000 to replay a normal firmware log.
"""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from fall_threshold_analysis import FallFSM, load_sessions, magnitude

ACTIONS = [
    ("静置", None),
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

FF_MG = 550.0
STILL_WIN_MS = 500.0


def mean_vec(samples):
    n = len(samples)
    if n == 0:
        return (0.0, 0.0, 0.0)
    return (sum(s[1] for s in samples) / n,
            sum(s[2] for s in samples) / n,
            sum(s[3] for s in samples) / n)


def angle_deg(a, b):
    na = math.sqrt(sum(i * i for i in a))
    nb = math.sqrt(sum(j * j for j in b))
    if na < 1.0 or nb < 1.0:
        return 0.0
    c = max(-1.0, min(1.0, sum(i * j for i, j in zip(a, b)) / (na * nb)))
    return math.degrees(math.acos(c))


def seg_features(samples, median_dt):
    mags = [magnitude(x, y, z) for (_, x, y, z) in samples]

    # longest continuous low-g run
    longest = 0
    run = 0
    for m in mags:
        if m < FF_MG:
            run += 1
            longest = max(longest, run)
        else:
            run = 0

    # terminal posture: last STILL_WIN_MS
    t_end = samples[-1][0]
    tail = [s for s in samples if t_end - s[0] <= STILL_WIN_MS]
    head = [s for s in samples if s[0] - samples[0][0] <= STILL_WIN_MS]
    g0 = mean_vec(head)
    g1 = mean_vec(tail)
    zxy = abs(g1[2]) / max(1.0, math.hypot(g1[0], g1[1]))
    tail_mags = [magnitude(x, y, z) for (_, x, y, z) in tail]
    mtail = sum(tail_mags) / len(tail_mags) if tail_mags else 0.0
    spread = math.sqrt(sum((v - mtail) ** 2 for v in tail_mags) /
                       len(tail_mags)) if tail_mags else 0.0

    return {
        "n": len(samples),
        "min": min(mags),
        "max": max(mags),
        "ff_ms": longest * median_dt,
        "angle": angle_deg(g0, g1),
        "zxy": zxy,
        "still": spread,
    }


def median_dt(samples):
    ds = [samples[i + 1][0] - samples[i][0] for i in range(len(samples) - 1)]
    ds = [d for d in ds if 0 < d < 1000]
    if not ds:
        return 0.0
    ds.sort()
    return float(ds[len(ds) // 2])


def quality(samples):
    dt = median_dt(samples)
    gaps = []
    for i in range(len(samples) - 1):
        d = samples[i + 1][0] - samples[i][0]
        if d > 200:
            gaps.append((samples[i][0], d))
    span = (samples[-1][0] - samples[0][0]) / 1000.0
    rate = len(samples) / span if span > 0 else 0.0
    return dt, gaps, span, rate


def run(args):
    sessions = load_sessions(args.log)
    if not sessions:
        print(f"{args.log}: 没有可用的 REC 数据")
        print("检查：烧的是不是 4.8.63-capture？串口是不是被投屏守护占着？")
        return 1

    # Analyse the longest session: a capture run may span a reboot.
    samples = max(sessions, key=len)
    if len(sessions) > 1:
        print(f"注意：日志含 {len(sessions)} 个 boot 段，分析最长的一段"
              f"（{len(samples)} 样本）")

    dt, gaps, span, rate = quality(samples)
    print("=== 采集质量")
    print(f"样本 {len(samples)}  时长 {span:.1f}s  中位间隔 {dt:.0f}ms  "
          f"速率 {rate:.1f} Hz")
    if gaps:
        print(f"断流 {len(gaps)} 处（>200ms），最长 "
              f"{max(g for _, g in gaps):.0f}ms：")
        for (t, g) in gaps[:8]:
            print(f"    t={t / 1000.0:6.1f}s  gap={g:.0f}ms")
        print("  断流处的动作特征不可信，该段结果标为 SKIP。")
    else:
        print("无断流，采样连续。")

    # Replay the whole session through the firmware state machine.
    fsm = FallFSM(args)
    for (t, x, y, z) in samples:
        fsm.feed(t, x, y, z)
    events = fsm.events
    print(f"\n状态机回灌：{len(events)} 次触发"
          f"（cooldown={args.cooldown:.0f}ms countdown={args.countdown:.0f}ms）")
    for (t, impact, ang) in events:
        print(f"    t={t / 1000.0:6.1f}s  impact={impact:.0f}mg  "
              f"angle={ang:.0f}deg")

    if args.scan:
        return scan(samples, dt, args)

    # ---- scripted timeline: idle, then one action per slot ----
    t0 = samples[0][0]
    print(f"\n=== 分段（idle {args.idle:.0f}s + 每 {args.slot:.0f}s 一个动作）")
    hdr = (f"{'#':>2}  {'动作':<26}{'样本':>5}{'|a|min':>8}{'|a|max':>8}"
           f"{'失重ms':>8}{'角度':>7}{'z/xy':>7}{'静止':>6}  {'触发':<5}"
           f"{'期望':<5}结果")
    print(hdr)

    tp = fp = fn = tn = skipped = 0
    for idx in range(len(ACTIONS)):
        label, expect = ACTIONS[idx]
        if idx == 0:
            lo, hi = 0.0, args.idle * 1000.0
        else:
            lo = (args.idle + (idx - 1) * args.slot) * 1000.0
            hi = lo + args.slot * 1000.0

        seg = [s for s in samples if lo <= (s[0] - t0) < hi]
        if len(seg) < 20:
            print(f"{idx:>2}  {label:<26}  样本不足，跳过")
            skipped += 1
            continue

        f = seg_features(seg, dt)
        # a detection belongs to this slot if its timestamp falls inside
        hit = [e for e in events
               if lo <= (e[0] - t0) < hi and e[0] >= seg[0][0]]
        seg_gap = any(lo <= (t - t0) < hi for (t, _) in gaps)

        fired = bool(hit)
        if expect is None:
            verdict = "-"
            if fired:
                verdict = "SETUP-FIRED"
        elif seg_gap:
            verdict = "SKIP"
            skipped += 1
        elif expect and fired:
            verdict = "OK"
            tp += 1
        elif expect and not fired:
            verdict = "MISS"
            fn += 1
        elif (not expect) and fired:
            verdict = "FALSE-ALARM"
            fp += 1
        else:
            verdict = "OK"
            tn += 1

        exp_txt = "否" if expect is False else ("是" if expect else "-")
        print(f"{idx:>2}  {label:<26}{f['n']:>5}{f['min']:>8.0f}"
              f"{f['max']:>8.0f}{f['ff_ms']:>8.0f}{f['angle']:>7.0f}"
              f"{f['zxy']:>7.2f}{f['still']:>6.0f}  "
              f"{'YES' if fired else '-':<5}{exp_txt:<5}{verdict}")

    print(f"\n=== 汇总  TP={tp} FP={fp} FN={fn} TN={tn} 跳过={skipped}")
    if fp:
        print("误报段：看它们的 |a|max 与 ff_ms —— 若 |a|max 明显低于真跌倒，"
              "冲击阈值可以收紧；若 ff_ms 短而成片，失重时长下限是更稳的判据。")
    if fn:
        print("漏报段：看 |a|max 是否触到 2400/2000，以及 angle 是否卡在 50° 边缘。")

    return 0


def scan(samples, dt, args):
    """Unlabelled sliding-window scan (use when the action timing slipped)."""

    win = args.scan * 1000.0
    t0 = samples[0][0]
    t_end = samples[-1][0]
    print(f"\n=== 滑窗扫描（{args.scan:.0f}s/窗）")
    print(f"{'t_start':>8}{'|a|min':>8}{'|a|max':>8}{'失重ms':>8}"
          f"{'角度':>7}{'z/xy':>7}")
    t = t0
    while t + win <= t_end:
        seg = [s for s in samples if t <= s[0] < t + win]
        if len(seg) >= 20:
            f = seg_features(seg, dt)
            flag = " <<<" if f["max"] > 2400 or f["min"] < FF_MG else ""
            print(f"{(t - t0) / 1000.0:>8.0f}{f['min']:>8.0f}{f['max']:>8.0f}"
                  f"{f['ff_ms']:>8.0f}{f['angle']:>7.0f}{f['zxy']:>7.2f}{flag}")
        t += win
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--idle", type=float, default=25.0)
    ap.add_argument("--slot", type=float, default=8.0)
    ap.add_argument("--scan", type=float, default=0.0,
                    help="滑窗秒数（不按动作表切分）")
    # Detection thresholds: keep the firmware defaults.
    ap.add_argument("--impact-hard", type=float, default=2400)
    ap.add_argument("--impact-soft", type=float, default=2000)
    ap.add_argument("--ff", type=float, default=550)
    ap.add_argument("--long-still", type=float, default=4500)
    ap.add_argument("--still-dev", type=float, default=80)
    ap.add_argument("--jolt", type=float, default=200)
    ap.add_argument("--angle", type=float, default=50)
    ap.add_argument("--window", type=float, default=3.0)
    # Quiet windows: capture build compresses both to 3 s.
    ap.add_argument("--cooldown", type=float, default=3000)
    ap.add_argument("--countdown", type=float, default=3000)
    args = ap.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
