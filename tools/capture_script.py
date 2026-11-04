#!/usr/bin/env python3
"""Scripted dataset capture plan -- single source of truth.

The three capture tools (capture_display.py, capture_monitor.py,
capture_analyze.py) all read the timeline from here, so the window you act
against and the offline segmentation can never drift apart.

Each action is (label, expect_detect, expect_steps, duration_s):

  expect_detect  True  -> a fall alarm should fire
                 False -> no alarm
                 None  -> setup/idle window, ignored in scoring
  expect_steps   minimum number of steps that should be counted in the slot
                 0 means "this action must NOT advance the step counter"
                 (that is what calibrates the step false positives)
  duration_s     how long the slot lasts; generous enough for slow actions

The step counter in the firmware is a pure magnitude threshold
(peak > 1150 mg, valley < 850 mg re-arms, 300 ms refractory), so every ADL
that swings the arm hard -- drinking, waving, typing, taking a coat off --
is expected to produce phantom steps.  The capture build now prints
<c>STEP,<t_ms>,<mag></c> per counted step, which turns each slot into one
labelled sample of that decision.
"""

IDLE_S = 25.0

ACTIONS = [
    # label,                              detect, steps, dur
    ("静置（戴好，手自然下垂，别动）",        None,  0, None),
    ("从桌上拿起手表 -> 戴上",              False, 0, 8),
    ("端杯子喝水（抬手到嘴边再放下）",        False, 0, 8),
    ("放下杯子（手臂落回桌面），再来一次",     False, 0, 8),
    ("站立 -> 侧倒在床垫",                 True,  0, 8),
    ("站立 -> 前倒在床垫",                 True,  0, 8),
    ("快速坐下（椅子）",                   False, 0, 8),
    ("重重坐下（屁股砸椅子）",              False, 0, 8),
    ("用手拍打手表 3 下",                  False, 0, 8),
    ("原地跳 2 下",                       False, 0, 8),
    ("正常走路 5 秒（计步基线）",           False, 1, 10),
    ("脱下手表放到桌上",                   False, 0, 8),
    ("从桌上拿起 -> 戴上",                 False, 0, 8),
    ("站立 -> 后倒在床垫",                 True,  0, 8),
    ("拿起放下 x3（误报复现）",             False, 0, 10),
    ("快步走 / 上下楼梯几步",               False, 1, 10),
    ("刷牙或挥手 10 秒",                   False, 0, 10),
    ("穿脱外套 / 整理衣袖",                 False, 0, 8),
    ("桌面打字 / 拍桌子 5 下",              False, 0, 8),
]


def timeline(idle=None, slot=None):
    """Return [(lo_ms, hi_ms), ...] per action index, relative to t0.

    ``slot`` forces a uniform slot length (used to re-analyse logs captured
    with an older build that used a fixed 8 s grid).
    """

    idle_s = IDLE_S if idle is None else float(idle)
    out = []
    t = 0.0
    for i, (_label, _det, _steps, dur) in enumerate(ACTIONS):
        if i == 0:
            length = idle_s
        else:
            length = float(slot) if slot is not None else float(dur)
        out.append((t * 1000.0, (t + length) * 1000.0))
        t += length
    return out


def total_s(idle=None, slot=None):
    tl = timeline(idle, slot)
    return tl[-1][1] / 1000.0


def action_at(elapsed_s, idle=None, slot=None):
    """Return (index, label, expect_detect, expect_steps, into_s, left_s).

    ``index`` is None once the script is over.
    """

    tl = timeline(idle, slot)
    ms = elapsed_s * 1000.0
    for idx, (lo, hi) in enumerate(tl):
        if ms < hi:
            label, det, steps, _dur = ACTIONS[idx]
            return (idx, label, det, steps, (ms - lo) / 1000.0,
                    (hi - ms) / 1000.0)
    return (None, None, None, None, 0.0, 0.0)


if __name__ == "__main__":
    print("总时长 %.0f s（静置 %.0f s + %d 个动作）"
          % (total_s(), IDLE_S, len(ACTIONS) - 1))
    for idx, (lo, hi) in enumerate(timeline()):
        print("%6.1f - %6.1f  %s"
              % (lo / 1000.0, hi / 1000.0, ACTIONS[idx][0]))
