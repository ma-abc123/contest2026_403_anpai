#!/usr/bin/env python3
"""Automated BLE connection regression test for the AI Watch.

Replicates the phone app's connect sequence from this host's BLE adapter
(hci0) and classifies each trial by the BOARD console (the authoritative
side):

    scan -> connect -> subscribe Status/DataUp CCCD -> write TimeSync

  SUCCESS   board printed "BLE: time synced"
  FAIL_HW   controller hardware-error / forced teardown before sync
  FAIL_DROP connection dropped before sync (no HW error)
  TIMEOUT   nothing conclusive within the window

Conditions (--load frames|none) toggle a continuous frame-request load
during each trial, mimicking the auto-preview viewer. This is the A/B
that separates "BLE is broken" from "BLE breaks under the preview load".

Usage:
    systemctl --user stop ai-watch-viewer     # free the serial port!
    python3 tools/ble_connect_test.py -n 5 --load none
    python3 tools/ble_connect_test.py -n 5 --load frames
"""

import argparse
import os
import select
import struct
import subprocess
import sys
import re
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from shot_protocol import wait_for_ready  # noqa: E402

SVC = "12345678-9abc-def0-1234-56789abcdef0"
UUID_STATUS = "12345678-9abc-def0-1234-56789abcdef1"
UUID_DATAUP = "12345678-9abc-def0-1234-56789abcdef3"
UUID_TIMESYNC = "12345678-9abc-def0-1234-56789abcdef2"

WATCH_NAME = "AI-Watch-403"


class ConsoleMonitor(threading.Thread):
    """Tail the board serial console, keep timestamped BLE-relevant lines.

    Frame bytes are consumed and discarded through the shared parser so
    the SHOT stream never blocks console parsing; console text between
    frames is line-split here.
    """

    EVENT_MARKS = ("BLE:", "aiwatch-ble:", "hardware-error", "Push page",
                   "Opened app", "controller recovery", "Force teardown")

    def __init__(self, ser):
        super().__init__(daemon=True)
        self.ser = ser
        self.lines = []            # (t, text)
        self.lock = threading.Lock()
        self.frame_tail = b""
        self.stop_flag = False

    def run(self):
        tail = b""
        while not self.stop_flag:
            try:
                data = self.ser.read(4096)
            except Exception:
                break
            if not data:
                time.sleep(0.01)
                continue
            data = self.frame_tail + data
            self.frame_tail = b""
            # drop SHOT frame payloads cheaply: find magic, skip 15+4len
            out = bytearray()
            i = 0
            while i < len(data):
                idx = data.find(b"SHOT", i)
                if idx < 0:
                    out.extend(data[i:])
                    break
                out.extend(data[i:idx])
                if len(data) - idx < 15:
                    self.frame_tail = bytes(data[idx:])
                    break
                import struct as _s
                _, _w, _h, _stride, _fmt, ln = _s.unpack(
                    "<4sHHHBI", data[idx:idx + 15])
                if 64 <= _w <= 1024 and 64 <= _h <= 1024 and \
                        0 < ln <= 2 * 1024 * 1024:
                    if len(data) - idx < 15 + ln:
                        self.frame_tail = bytes(data[idx:])
                        break
                    i = idx + 15 + ln
                else:
                    out.extend(data[idx:idx + 4])
                    i = idx + 4
            for line in bytes(out).split(b"\n"):
                line = line.decode("latin-1", "replace").strip()
                if line and any(m in line for m in self.EVENT_MARKS):
                    with self.lock:
                        self.lines.append((time.time(), line))

    def since(self, t):
        with self.lock:
            return [(ts, l) for (ts, l) in self.lines if ts >= t]

    def clear(self):
        with self.lock:
            self.lines.clear()


class Bluetoothctl:
    """Thin pipe driver for `bluetoothctl` interactive mode."""

    def __init__(self):
        self.p = subprocess.Popen(
            ["bluetoothctl"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=0)

    def send(self, cmd, wait=0.8):
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()
        t_end = time.time() + wait
        out = []
        while time.time() < t_end:
            r, _, _ = select.select([self.p.stdout], [], [], 0.1)
            if r:
                line = self.p.stdout.readline()
                if line:
                    out.append(line.rstrip())
        return out

    def wait_for(self, marks, timeout, collect=None):
        """Read until any mark appears; return (matched, all_lines)."""

        t_end = time.time() + timeout
        all_lines = []
        while time.time() < t_end:
            r, _, _ = select.select([self.p.stdout], [], [], 0.1)
            if r:
                line = self.p.stdout.readline()
                if not line:
                    continue
                line = line.rstrip()
                if collect is not None:
                    all_lines.append(line)
                for m in marks:
                    if m in line:
                        return m, all_lines
        return None, all_lines

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except Exception:
            pass
        time.sleep(0.2)
        self.p.kill()


def timesync_value():
    utc = int(time.time())
    tz_min = 480                      # UTC+8, matches the phone app
    return struct.pack("<BiH", 1, utc, tz_min & 0xFFFF).hex()


class LoadThread(threading.Thread):
    """Continuously request frames, one outstanding, like the viewer."""

    def __init__(self, ser):
        super().__init__(daemon=True)
        self.ser = ser
        self.stop_flag = False

    def run(self):
        while not self.stop_flag:
            try:
                self.ser.write(b"s")
                self.ser.flush()
                time.sleep(0.05)
                self.ser.read(4096)     # drain; content irrelevant here
            except Exception:
                break


def one_trial(bt, console, mac, load, trial, gap_s):
    print(f"--- trial {trial}: load={load} ---")
    console.clear()
    t0 = time.time()

    bt.send("scan on", 0.3)
    m, _ = bt.wait_for([WATCH_NAME], 8.0)
    bt.send("scan off", 0.3)
    if m is None:
        evs = console.since(t0 - 1)
        print(f"   未扫到板子（未在广播？）匹配行: {m!r:.120}")
        print("   板端近况:")
        for (_, l) in evs[-6:]:
            print(f"     | {l[:150]}")
        return "NO_ADV", 0.0, []

    t0 = time.time()
    load_thr = None
    if load == "frames":
        load_thr = LoadThread(console.ser)
        load_thr.start()

    bt.send(f"connect {mac}", 0.3)
    m, _ = bt.wait_for(["Connection successful", "Failed to connect",
                        "Failed"], 15.0)
    t_conn = time.time() - t0 if m == "Connection successful" else None
    if m != "Connection successful":
        if load_thr:
            load_thr.stop_flag = True
        print(f"   连接失败: {m}")
        return "FAIL_CONN", 0.0, []

    # subscribe Status + DataUp, then write TimeSync -- the phone's order
    bt.send("menu gatt", 0.5)
    for uuid in (UUID_STATUS, UUID_DATAUP):
        bt.send(f"select-attribute {uuid}", 0.4)
        bt.send("notify on", 0.4)
    bt.send(f"select-attribute {UUID_TIMESYNC}", 0.4)
    bt.send(f"write {timesync_value()}", 0.8)

    # wait for the authoritative board verdict
    verdict = "TIMEOUT"
    t_end = time.time() + 10.0
    events = []
    while time.time() < t_end:
        evs = console.since(t0)
        events = [l for (_, l) in evs]
        if any("time synced" in l for l in events):
            verdict = "SUCCESS"
            break
        if any("hardware-error" in l or "Force teardown" in l
               for l in events):
            verdict = "FAIL_HW"
            break
        if any("disconnected, reason" in l for l in events):
            verdict = "FAIL_DROP"
            break
        time.sleep(0.05)

    if load_thr:
        load_thr.stop_flag = True
        load_thr.join(timeout=1)

    bt.send("disconnect", 1.0)
    bt.send("back", 0.3)
    time.sleep(gap_s)
    return verdict, t_conn, events


def mac_from_line(line):
    m = re.search(r"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){6})", line)
    return m.group(1) if m else None



# The auto-preview daemon holds the same serial port; concurrent readers
# split the byte stream, so refuse to run rather than fail mysteriously.
pidfile = "/tmp/aiwatch_viewer.pid"
if os.path.exists(pidfile):
    try:
        pid = int(open(pidfile).read().strip())
        os.kill(pid, 0)
        print(f"自动投屏守护正在运行 (pid {pid})，与测试抢串口。")
        print("先停: systemctl --user stop ai-watch-viewer")
        sys.exit(2)
    except (ValueError, ProcessLookupError, PermissionError):
        pass

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=1000000)
    ap.add_argument("-n", "--trials", type=int, default=5)
    ap.add_argument("--load", choices=["none", "frames"], default="none")
    ap.add_argument("--mac", default=None)
    ap.add_argument("--gap", type=float, default=4.0)
    args = ap.parse_args()

    import serial
    ser = serial.Serial(args.port, args.baud, timeout=0)
    ser.rts = False
    ser.dtr = False
    time.sleep(0.3)
    ser.reset_input_buffer()
    ready, _banner_bytes = wait_for_ready(ser.read, timeout=15.0)
    print("板子就绪" if ready else "未见启动横幅（按已运行处理）")
    ser.reset_input_buffer()
    ser.write(b"v")
    time.sleep(0.2)
    ser.reset_input_buffer()

    console = ConsoleMonitor(ser)
    console.start()

    bt = Bluetoothctl()
    bt.send("power on", 1.5)

    mac = args.mac
    if not mac:
        mac = None
        for attempt in range(3):
            bt.send("scan on", 0.3)
            t_end = time.time() + 12.0
            seen = []
            while time.time() < t_end:
                r, _, _ = select.select([bt.p.stdout], [], [], 0.1)
                if not r:
                    continue
                line = bt.p.stdout.readline()
                if not line:
                    continue
                line = line.rstrip()
                if "Device" in line:
                    seen.append(line)
                if WATCH_NAME in line and mac is None:
                    mac = mac_from_line(line)
            bt.send("scan off", 0.3)
            if mac:
                break
            print(f"  初扫第 {attempt + 1} 次未取得 MAC，重试..."
                  f"（本窗口看到 {len(seen)} 个设备）")
            for l in seen[:5]:
                print(f"    · {l[:100]}")
    print(f"目标 {WATCH_NAME} @ {mac}")

    verdicts = []
    try:
        i = 0
        dark_boots = 0
        while i < args.trials:
            v, t_conn, events = one_trial(bt, console, mac, args.load, i + 1,
                                          args.gap)
            if v == "NO_ADV" and dark_boots < 2:
                # this boot never advertised; a fresh reset re-rolls the
                # bring-up dice (measured 2026-10-31: 1/3 of boots come up
                # late or never within the scan window). Reopening the
                # port is what pulses DTR and resets the board.
                dark_boots += 1
                print(f"   暗 boot #{dark_boots}，重开串口换一个 boot...")
                ser.close()
                time.sleep(0.5)
                ser.open()
                ser.rts = False
                ser.dtr = False
                time.sleep(0.3)
                ser.reset_input_buffer()
                ready, _ = wait_for_ready(ser.read, timeout=15.0)
                print("   新 boot 就绪" if ready else "   新 boot 未就绪")
                ser.reset_input_buffer()
                continue
            verdicts.append(v)
            i += 1
            tail = " | ".join(events[-3:]) if events else ""
            print(f"   => {v} (connect={t_conn}s)" if t_conn else f"   => {v}")
            if tail:
                print(f"      板端: {tail[:200]}")
    finally:
        bt.close()

    ok = verdicts.count("SUCCESS")
    print("\n== 汇总 ==")
    from collections import Counter
    for k, c in Counter(verdicts).items():
        print(f"  {k}: {c}")
    return 0 if ok == len(verdicts) else 1


if __name__ == "__main__":
    sys.exit(main())
