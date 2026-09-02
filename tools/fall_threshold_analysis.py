#!/usr/bin/env python3
"""Offline threshold analysis for the AI Watch fall detector (M4).

Feeds serial-captured REC lines back through the same fall state machine
that runs on the watch, so thresholds can be tuned offline before
touching the firmware:

    REC,<t_ms>,<x_mg>,<y_mg>,<z_mg>,<gx_mdps>,<gy_mdps>,<gy_mdps>

Input files can be raw serial logs - every line matching the REC pattern
is used, everything else is ignored.

Usage:
    python3 fall_threshold_analysis.py rest.csv walk.csv fall1.csv \
        [--impact-hard 2800] [--impact-soft 2200] [--still-dev 80]
        [--angle 50] [--window 3.0]

Naming convention for quick verdicts:
    files with "fall" in the name are expected to trigger one event,
    all other files are expected to trigger none.
"""

import argparse
import math
import re
import sys

REC_RE = re.compile(
    r"REC,(\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+)\s*$")

GRAVITY_MG = 1000.0


def load_sessions(path):
    """Parse REC lines into sessions.

    One serial capture usually contains several Record sessions; the
    firmware restarts t_ms at 0 for each, so a timestamp that goes
    backwards starts a new session.
    """

    sessions = []
    current = []
    prev_t = 0

    with open(path, "r", errors="replace") as f:
        for line in f:
            m = REC_RE.search(line)
            if not m:
                continue
            t = int(m.group(1))
            if t < prev_t and current:
                sessions.append(current)
                current = []
            prev_t = t
            x, y, z = (int(m.group(i)) for i in (2, 3, 4))
            current.append((t, x, y, z))

    if current:
        sessions.append(current)

    # Drop fragments too short to say anything about

    return [s for s in sessions if len(s) >= 20]


def magnitude(x, y, z):
    return math.sqrt(x * x + y * y + z * z)


class FallFSM:
    """Mirror of the firmware state machine (NORMAL/IMPACT/ALARM).

    Impact rule (any of): single sample above impact_hard; two
    consecutive samples above impact_soft; a free-fall sample
    (< ff) within the last 500 ms followed by a sample above
    impact_soft. The pre-impact bearing is the fast gravity LPF
    frozen at the impact sample.
    """

    def __init__(self, args):
        self.args = args
        self.reset()

    def reset(self):
        self.state = "NORMAL"
        self.window_end = 0.0
        self.cooldown = 0.0
        self.soft_prev = False
        self.pre_bearing = (0.0, 0.0, GRAVITY_MG)
        self.grav_f = [0.0, 0.0, GRAVITY_MG]
        self.ff_seen_ms = 0          # 0 = nothing latched yet
        self.imp_ff = False          # free-fall preceded the impact
        self.imp_start = 0.0         # impact time (observation budget)
        self.imp_last_move = 0.0     # last sample dev > still_dev
        self.dev_ring = []           # (t, |a|-mean)
        self.mean = GRAVITY_MG
        self.events = []

    def angle_deg(self, a, b):
        dot = sum(i * j for i, j in zip(a, b))
        na = math.sqrt(sum(i * i for i in a))
        nb = math.sqrt(sum(j * j for j in b))
        if na < 1.0 or nb < 1.0:
            return 0.0
        c = max(-1.0, min(1.0, dot / (na * nb)))
        return math.degrees(math.acos(c))

    def feed(self, t_ms, x, y, z):
        a = self.args
        mag = magnitude(x, y, z)
        dev = abs(mag - self.mean)
        self.mean += 0.01 * (mag - self.mean)

        for i in range(3):
            v = (x, y, z)[i]
            self.grav_f[i] += 0.15 * (v - self.grav_f[i])

        if mag < a.ff:
            self.ff_seen_ms = t_ms

        self.dev_ring.append((t_ms, dev))
        if len(self.dev_ring) > 40:
            self.dev_ring.pop(0)

        if self.state == "NORMAL":
            ff_recent = (self.ff_seen_ms != 0 and
                         t_ms - self.ff_seen_ms <= 500)
            impact = (mag > a.impact_hard or
                      (self.soft_prev and mag > a.impact_soft) or
                      (ff_recent and mag > a.impact_soft))
            self.soft_prev = mag > a.impact_soft

            if impact and t_ms >= self.cooldown:
                self.state = "IMPACT"
                self.window_end = t_ms + a.window * 1000.0
                self.pre_bearing = tuple(self.grav_f)
                self.impact_mag = mag
                self.imp_ff = ff_recent
                self.imp_start = t_ms
                self.imp_last_move = t_ms
            return

        if self.state == "IMPACT":
            if dev > a.still_dev:
                self.imp_last_move = t_ms

            self.impact_mag = max(self.impact_mag, mag)  # true peak

            if dev > a.jolt or mag > a.impact_soft:
                self.window_end = t_ms + a.window * 1000.0

            if t_ms < self.window_end:
                return

            recent = [d for (t, d) in self.dev_ring
                      if t_ms - t <= 1000]
            still = bool(recent) and max(recent) <= a.still_dev

            if not still:
                self.state = "NORMAL"
                self.cooldown = t_ms + 60000
                return

            ang = self.angle_deg(self.pre_bearing, self.grav_f)

            if ang >= a.angle:
                self.events.append((t_ms, self.impact_mag, ang))
                self.state = "ALARM-DONE"   # reported; stays until reset
                return

            if self.imp_ff and t_ms - self.imp_last_move >= a.long_still:
                # Dropped flat without rotating: free-fall + long
                # stillness (the alternate confirmation path)

                self.events.append((t_ms, self.impact_mag, ang))
                self.state = "ALARM-DONE"
                return

            if t_ms - self.imp_start < 10000:
                self.window_end = t_ms + 1000   # keep watching
                return

            self.state = "NORMAL"
            self.cooldown = t_ms + 60000


def analyze(path, args):
    sessions = load_sessions(path)
    if not sessions:
        print(f"\n=== {path}: no usable REC sessions found")
        return

    for idx, samples in enumerate(sessions, 1):
        mags = [magnitude(x, y, z) for (_, x, y, z) in samples]
        duration = samples[-1][0] / 1000.0
        rate = len(samples) / duration if duration > 0 else 0.0
        label = f"{path} [session {idx}/{len(sessions)}]"

        print(f"\n=== {label}")
        print(f"    samples={len(samples)}  duration={duration:.1f}s  "
              f"rate={rate:.1f} Hz")
        print(f"    |a| min/mean/max = {min(mags):.0f}/"
              f"{sum(mags) / len(mags):.0f}/{max(mags):.0f} mg")

        fsm = FallFSM(args)
        for (t, x, y, z) in samples:
            fsm.feed(t, x, y, z)

        if fsm.events:
            for (t, impact, ang) in fsm.events:
                print(f"    FALL EVENT at t={t / 1000.0:.1f}s  "
                      f"impact={impact:.0f} mg  angle={ang:.0f} deg")
        else:
            print("    no fall events")

        expect = "fall" in path.lower()
        detected = bool(fsm.events)
        verdict = "PASS" if expect == detected else "FAIL"
        print(f"    expected={'fall' if expect else 'no-fall'}  "
              f"detected={detected}  -> {verdict}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("files", nargs="+")
    p.add_argument("--impact-hard", type=float, default=2400)
    p.add_argument("--impact-soft", type=float, default=2000)
    p.add_argument("--ff", type=float, default=550)
    p.add_argument("--long-still", type=float, default=4500)
    p.add_argument("--still-dev", type=float, default=80)
    p.add_argument("--jolt", type=float, default=200)
    p.add_argument("--angle", type=float, default=50)
    p.add_argument("--window", type=float, default=3.0)
    args = p.parse_args()

    for path in args.files:
        analyze(path, args)

    return 0


if __name__ == "__main__":
    sys.exit(main())
