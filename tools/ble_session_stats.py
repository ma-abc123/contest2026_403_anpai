#!/usr/bin/env python3
"""BLE session statistics from the serial console.

Read-only: opens the port, releases DTR/RTS (same path as `cap_boot.py noreset`)
and then only reads. Nothing is written to the board and the SoC is not reset,
so the script can be started while a phone is already connected.

Counts what the firmware already logs:

  aiwatch-ble: advertising as "AI-Watch-403"
  aiwatch-ble: connected
  aiwatch-ble: disconnected, reason 0x13 duration_ms=15234 state=3
  aiwatch-ble: controller recovery type=...
  <err> Hardware error, hardware code: 0
  aiwatch-ble: advertising start failed: -110

Usage:
  python3 tools/ble_session_stats.py                 # run until Ctrl-C
  python3 tools/ble_session_stats.py -d 300          # 5 minutes
  python3 tools/ble_session_stats.py -p /dev/ttyUSB0 -o run.csv

A connection shorter than --short-ms is counted as a short link (the phone
gave up or the controller dropped it early); the rest count as sustained.
"""

import argparse
import os
import re
import statistics
import sys
import time

import serial

RE_ADV = re.compile(r"aiwatch-ble: advertising as")
RE_CONN = re.compile(r"aiwatch-ble: connected")
RE_DISC = re.compile(
    r"aiwatch-ble: disconnected, reason (0x[0-9a-fA-F]+) duration_ms=(\d+)"
)
RE_RECOV = re.compile(r"aiwatch-ble: controller recovery")
RE_HWERR = re.compile(r"Hardware error, hardware code: (\d+)")
RE_ADVFAIL = re.compile(r"advertising start failed")


class Stats:
    def __init__(self, short_ms):
        self.short_ms = short_ms
        self.adv = 0
        self.conn = 0
        self.durations = []
        self.reasons = {}
        self.recovery = 0
        self.hwerr = {}
        self.advfail = 0
        self.started = time.time()

    def feed(self, line):
        hit = False

        if RE_ADV.search(line):
            self.adv += 1
            hit = True

        if RE_CONN.search(line):
            self.conn += 1
            hit = True

        m = RE_DISC.search(line)
        if m:
            reason, dur = m.group(1).lower(), int(m.group(2))
            self.durations.append(dur)
            self.reasons[reason] = self.reasons.get(reason, 0) + 1
            hit = True

        if RE_RECOV.search(line):
            self.recovery += 1
            hit = True

        m = RE_HWERR.search(line)
        if m:
            code = m.group(1)
            self.hwerr[code] = self.hwerr.get(code, 0) + 1
            hit = True

        if RE_ADVFAIL.search(line):
            self.advfail += 1
            hit = True

        return hit

    def report(self, final=False):
        elapsed = time.time() - self.started
        short = [d for d in self.durations if d < self.short_ms]
        long = [d for d in self.durations if d >= self.short_ms]

        out = []
        out.append(f"=== BLE session {elapsed:.0f}s "
                   f"{'(final)' if final else '(running)'} ===")
        out.append(f"广告 {self.adv} 次，连接建立 {self.conn} 次，"
                   f"断开记录 {len(self.durations)} 次")

        if self.durations:
            out.append(f"维持 {len(long)} 次（>= {self.short_ms} ms），"
                       f"短连 {len(short)} 次")
            if len(long) == len(self.durations):
                rate = 100.0
            else:
                rate = 100.0 * len(long) / len(self.durations)
            out.append(f"成功率 {rate:.0f}%")

            s = sorted(self.durations)
            med = statistics.median(s)
            out.append(f"连接时长 min/median/max: "
                       f"{s[0] / 1000:.1f} / {med / 1000:.1f} / "
                       f"{s[-1] / 1000:.1f} s")

            why = ", ".join(f"{k}×{v}" for k, v in sorted(self.reasons.items()))
            out.append(f"断开原因: {why}")
        else:
            out.append("还没有断开记录（连上后没断，或尚未连上）")

        extras = []
        if self.recovery:
            extras.append(f"恢复 {self.recovery} 次")
        if self.hwerr:
            codes = ", ".join(f"code {k}×{v}" for k, v in self.hwerr.items())
            extras.append(f"硬件错误 {codes}")
        if self.advfail:
            extras.append(f"广播失败 {self.advfail} 次")
        if extras:
            out.append("；".join(extras))

        return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=1000000)
    ap.add_argument("-d", "--duration", type=float, default=0,
                    help="seconds to run, 0 = until Ctrl-C")
    ap.add_argument("-o", "--out", default="",
                    help="optional raw log file")
    ap.add_argument("--short-ms", type=int, default=3000,
                    help="links shorter than this count as short")
    ap.add_argument("--every", type=float, default=30,
                    help="seconds between running summaries, 0 = off")
    args = ap.parse_args()

    st = Stats(args.short_ms)
    out = open(args.out, "a") if args.out else None

    def open_port():
        # NOTE: opening the port asserts DTR/RTS, and on this board RTS high
        # holds the SoC in reset. Releasing it right away costs one reset --
        # unavoidable, it is how the UART is wired. What matters is that we do
        # this once and then keep the handle open, so a running session is
        # never reset mid-test.
        port = serial.Serial(args.port, args.baud, timeout=0.2)
        port.rts = False
        port.dtr = False
        time.sleep(0.2)
        port.reset_input_buffer()
        return port

    ser = None
    misses = 0
    try:
        ser = open_port()
    except Exception as exc:  # noqa: BLE001
        # Not fatal: the board may still be unplugged. Keep waiting for it
        # instead of exiting, so the user can start the script first.
        print(f"{args.port} 暂不可用（{exc}），等待设备出现...", flush=True)

    print(st.report())
    print("--- 监听中，Ctrl-C 结束 ---", flush=True)

    deadline = time.time() + args.duration if args.duration else None
    next_summary = time.time() + args.every if args.every else None

    try:
        while True:
            if deadline and time.time() >= deadline:
                break

            # A USB replug makes the handle stale; reopen and carry on with
            # the counters intact. Reopening is only ever triggered by the
            # device actually disappearing, because each open costs a reset.
            if ser is None:
                if not os.path.exists(args.port):
                    time.sleep(1.0)
                    continue
                try:
                    ser = open_port()
                except Exception:  # noqa: BLE001
                    time.sleep(1.0)
                    continue
                print("--- 串口已连上（板子会复位一次）---", flush=True)
                misses = 0
                continue

            try:
                data = ser.read(4096)
                misses = 0
            except (serial.SerialException, OSError):
                # One hiccup is not a disconnect; only give up the handle
                # after repeated failures, otherwise we would reset the board
                # for a transient read error.
                misses += 1
                if misses < 3:
                    time.sleep(0.3)
                    continue
                print("--- 串口断开，等待重连 ---", flush=True)
                try:
                    ser.close()
                except Exception:  # noqa: BLE001
                    pass
                ser = None
                time.sleep(1.0)
                continue

            if data:
                text = data.decode("utf-8", "replace")
                if out:
                    out.write(text)
                    out.flush()
                for line in text.splitlines():
                    if st.feed(line):
                        print("  " + line.strip(), flush=True)

            if next_summary and time.time() >= next_summary:
                print(st.report(), flush=True)
                next_summary = time.time() + args.every
    except KeyboardInterrupt:
        pass
    finally:
        print(st.report(final=True))
        if ser is not None:
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
        if out:
            out.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
