#!/usr/bin/env python3
"""Per-boot BLE advertising lottery test.

Opening the serial port resets the board, so each session is one boot.
For each boot: wait for the app banner, then scan over hci0 for 12 s and
record whether "AI-Watch-403" was discovered. Repeats N boots and prints
the failure rate -- the regression gate for the LCPU bring-up wedge
family (boots that never advertise).

    python3 tools/ble_adv_lottery.py -n 8
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from shot_protocol import wait_for_ready  # noqa: E402


def one_boot(port, baud, scan_s, keep_open=False, send_v=False):
    import serial
    ser = serial.Serial(port, baud, timeout=0)
    ser.rts = False
    ser.dtr = False
    time.sleep(0.3)
    ser.reset_input_buffer()
    ready, banner = wait_for_ready(ser.read, timeout=15.0)
    adv_in_banner = b"advertising as" in banner
    if send_v:
        ser.write(b"v")
        time.sleep(0.3)

    # drain console in the background while the host scans
    console = {"buf": bytearray(), "stop": False}

    def reader():
        tail = b""
        while not console["stop"]:
            try:
                data = ser.read(4096)
            except Exception:
                break
            if not data:
                time.sleep(0.02)
                continue
            data = tail + data
            tail = b""
            out = bytearray()
            i = 0
            while i < len(data):
                idx = data.find(b"SHOT", i)
                if idx < 0:
                    out.extend(data[i:])
                    break
                out.extend(data[i:idx])
                if len(data) - idx < 15:
                    tail = bytes(data[idx:])
                    break
                _, w, h, st, fmt, ln = struct.unpack(
                    "<4sHHHBI", data[idx:idx + 15])
                if (64 <= w <= 1024 and 64 <= h <= 1024 and
                        0 < ln <= 2 * 1024 * 1024):
                    if len(data) - idx < 15 + ln:
                        tail = bytes(data[idx:])
                        break
                    i = idx + 15 + ln
                else:
                    out.extend(data[idx:idx + 4])
                    i = idx + 4
            console["buf"].extend(out)

    thr = threading.Thread(target=reader, daemon=True)
    thr.start()

    # scan in a fresh bluetoothctl session with stdin kept open
    p = subprocess.Popen(["bluetoothctl"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, bufsize=0)

    def send(cmd, wait=0.6):
        p.stdin.write(cmd + "\n")
        p.stdin.flush()
        time.sleep(wait)

    send("power on", 1.0)
    send("scan on", 0.3)
    t_end = time.time() + scan_s
    found = False
    t_found = None
    while time.time() < t_end:
        r, _, _ = __import__("select").select([p.stdout], [], [], 0.1)
        if r:
            line = p.stdout.readline()
            if line and "AI-Watch-403" in line:
                found = True
                t_found = time.time() - t_end + scan_s
                break
    send("scan off", 0.3)
    send("quit", 0.2)
    p.kill()
    console["stop"] = True
    thr.join(timeout=1)
    if not keep_open:
        ser.close()

    text = bytes(console["buf"]).replace(b"\r", b"").decode(
        "latin-1", "replace")
    pat = re.compile(r"ble:|advertis|assert|recovery|teardown|HCI|patch|"
                     r"lcpu release|IPC queue", re.I)
    console_lines = [l.strip() for l in text.split("\n") if pat.search(l)]
    try:
        ser.close()
    except Exception:
        pass
    return ready, adv_in_banner, found, t_found, console_lines



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
    ap.add_argument("-n", "--boots", type=int, default=8)
    ap.add_argument("--scan-s", type=float, default=12.0)
    ap.add_argument("--keep-open", action="store_true")
    ap.add_argument("--send-v", action="store_true")
    args = ap.parse_args()

    results = []
    for i in range(1, args.boots + 1):
        ready, adv_banner, found, t_found, clines = one_boot(
            args.port, args.baud, args.scan_s,
            keep_open=args.keep_open, send_v=args.send_v)
        results.append(found)
        print(f"boot {i}: ready={ready} banner_adv={adv_banner} "
              f"discovered={found}"
              + (f" @ {t_found:.1f}s" if t_found is not None else ""),
              flush=True)
        if not found:
            for l in clines[-8:]:
                print(f"   | {l[:140]}", flush=True)
        time.sleep(1.0)

    n_ok = sum(results)
    print(f"\n== 汇总: {n_ok}/{len(results)} boots 广播可见 ==")
    return 0 if n_ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
