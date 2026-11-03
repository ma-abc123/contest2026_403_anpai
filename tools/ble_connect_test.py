#!/usr/bin/env python3
"""AI Watch BLE 连接/重连回归：实时扫描 -> CCC -> 校时 -> 存活观察。

用 /usr/bin/python3（需 bleak、pyserial）；先停 ai-watch-viewer。
每轮都重新收取广播获取当前随机地址，按 UUID 解析 GATT；不使用
bluetoothctl 输出或缓存中的设备名判断是否在广播。只保留一个串口读者。

  /usr/bin/python3 tools/ble_connect_test.py -n 5 --hold 45 --load none
  /usr/bin/python3 tools/ble_connect_test.py -n 5 --hold 45 --load frames

本工具不主动复位、不重开串口；NO_ADV 是实际扫描窗口的结果，不自动
丢弃失败轮次。打开串口前预置 DTR/RTS=False，仍应通过启动日志判断
USB 驱动是否产生了复位毛刺。重测开机请外部复位后运行，或启动工具后
按板上复位键。--startup-wait 只等待、不发送任何板端命令。

保存 <log-prefix>.log（板端完整文本、主机阶段、每轮结果），.raw（逐字节
原始串口，含帧）和 .chunks.jsonl（原始块的 UTC/单调时间及 offset/length）。
投屏帧中被插入的日志无法可靠分离，保留 raw 用于复核；链路存活同时检查
BlueZ 断连事件与板端断连/控制器故障，sync 模式还必须看到板端校时确认。
"""

import argparse
import asyncio
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import struct
import threading
import time

SVC = "12345678-9abc-def0-1234-56789abcdef0"
UUID_STATUS = "12345678-9abc-def0-1234-56789abcdef1"
UUID_DATAUP = "12345678-9abc-def0-1234-56789abcdef3"
UUID_TIMESYNC = "12345678-9abc-def0-1234-56789abcdef2"
WATCH_NAME = "AI-Watch-403"
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


class RunLog:
    def __init__(self, prefix):
        prefix = Path(prefix).expanduser()
        prefix.parent.mkdir(parents=True, exist_ok=True)
        self.prefix = str(prefix)
        self.text = open(self.prefix + ".log", "w", encoding="utf-8", buffering=1)
        self.lock = threading.Lock()

    def write(self, source, text, echo=False):
        stamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        line = f"[{stamp}] {source}: {text}"
        with self.lock:
            self.text.write(line + "\n")
            if echo:
                print(line, flush=True)

    def close(self):
        self.text.close()


class FrameFilter:
    """Strip SHOT bodies without decoding, keeping split headers/magic."""

    def __init__(self, sink):
        self.sink = sink
        self.pending = bytearray()
        self.remaining = 0
        self.frames = 0

    def feed(self, data):
        self.pending.extend(data)
        while self.pending:
            if self.remaining:
                n = min(self.remaining, len(self.pending))
                del self.pending[:n]
                self.remaining -= n
                if self.remaining == 0:
                    self.frames += 1
                continue
            idx = self.pending.find(b"SHOT")
            if idx < 0:
                keep = next((n for n in (3, 2, 1)
                             if self.pending.endswith(b"SHOT"[:n])), 0)
                end = len(self.pending) - keep
                self.sink(bytes(self.pending[:end]))
                del self.pending[:end]
                break
            if idx:
                self.sink(bytes(self.pending[:idx]))
                del self.pending[:idx]
            if len(self.pending) < 15:
                break
            _, width, height, stride, fmt, length = struct.unpack(
                "<4sHHHBI", self.pending[:15])
            if (64 <= width <= 1024 and 64 <= height <= 1024 and
                    stride >= width * 2 and fmt in (1, 2) and
                    0 < length <= 2 * 1024 * 1024):
                del self.pending[:15]
                self.remaining = length
            else:
                self.sink(bytes(self.pending[:4]))
                del self.pending[:4]


class ConsoleMonitor(threading.Thread):
    """The only serial reader for the entire run; never reopen underneath it."""

    def __init__(self, ser, log):
        super().__init__(daemon=True)
        self.ser = ser
        self.log = log
        self.stop_flag = threading.Event()
        self.lock = threading.Lock()
        self.lines = []
        self.line_tail = b""
        self.error = None
        self.filter = FrameFilter(self._text)
        self.raw = open(log.prefix + ".raw", "wb")
        self.chunks = open(log.prefix + ".chunks.jsonl", "w", buffering=1)
        self.offset = 0

    def _text(self, data):
        lines = (self.line_tail + data).split(b"\n")
        self.line_tail = lines.pop()
        # Bound corrupt/binary text after a torn frame without dropping raw.
        if len(self.line_tail) > 65536:
            lines.append(self.line_tail)
            self.line_tail = b""
        for part in lines:
            text = ANSI.sub("", part.decode("latin-1", "replace")).strip("\r\0 ")
            if text:
                now = time.monotonic()
                with self.lock:
                    self.lines.append((now, text))
                self.log.write("BOARD", text)

    def run(self):
        try:
            while not self.stop_flag.is_set():
                data = self.ser.read(8192)
                if not data:
                    continue
                self.raw.write(data)
                self.chunks.write(json.dumps({
                    "utc": datetime.now(timezone.utc).isoformat(),
                    "monotonic": time.monotonic(),
                    "offset": self.offset, "length": len(data),
                }) + "\n")
                self.offset += len(data)
                self.filter.feed(data)
        except Exception as exc:
            self.error = repr(exc)
            self.log.write("SERIAL_ERROR", self.error, echo=True)
        finally:
            if self.line_tail:
                self.log.write("BOARD_PARTIAL", self.line_tail.decode(
                    "latin-1", "replace"))
            self.raw.close()
            self.chunks.close()

    def since(self, stamp):
        with self.lock:
            return [line for ts, line in self.lines if ts >= stamp]


def fault_from_lines(lines):
    for line in lines:
        lower = line.lower()
        if any(mark in lower for mark in (
                "hardware error", "hardware-error", "controller recovery",
                "force teardown", "force host teardown", "controller unresponsive")):
            return "FAIL_HW", line
    for line in lines:
        if "disconnected, reason" in line:
            return "FAIL_DROP", line
    return None, ""


def link_fault(console, stamp, disconnected, client):
    if console.error:
        return "SERIAL_ERROR", console.error
    fault, detail = fault_from_lines(console.since(stamp))
    if fault:
        return fault, detail
    if disconnected.is_set() or not client.is_connected:
        return "FAIL_DROP", "BlueZ disconnected"
    return None, ""


async def find_watch(args, log):
    from bleak import BleakScanner

    event = asyncio.Event()
    found = None
    seen = set()

    def advertisement(device, adv):
        nonlocal found
        seen.add(device.address)
        name = adv.local_name or device.name or ""
        matches = (device.address.lower() == args.mac.lower() if args.mac else
                   name == WATCH_NAME or SVC in [s.lower() for s in adv.service_uuids])
        if matches:
            found = device
            log.write("ADV", f"{device.address} name={name!r} rssi={adv.rssi}")
            event.set()

    async with BleakScanner(advertisement, adapter=args.adapter):
        try:
            await asyncio.wait_for(event.wait(), args.scan_s)
        except asyncio.TimeoutError:
            pass
    log.write("SCAN", f"seen={len(seen)} target={getattr(found, 'address', None)}",
              echo=True)
    return found


async def frame_load(ser, console, log):
    """One request at a time; only ConsoleMonitor consumes response bytes."""
    try:
        while True:
            count = console.filter.frames
            ser.write(b"s")
            deadline = time.monotonic() + 2.0
            while (console.filter.frames == count and
                   time.monotonic() < deadline and not console.error):
                await asyncio.sleep(0.02)
            await asyncio.sleep(0.05)
    except asyncio.CancelledError:
        raise
    except Exception as exc:
        console.error = f"frame writer: {exc!r}"
        log.write("SERIAL_ERROR", console.error, echo=True)


async def one_trial(args, console, log, trial):
    from bleak import BleakClient

    result = {"trial": trial, "load": args.load, "handshake": args.handshake}
    log.write("TRIAL", f"{trial}/{args.trials} load={args.load}", echo=True)
    try:
        device = await find_watch(args, log)
    except Exception as exc:
        return dict(result, verdict="SCAN_ERROR", detail=repr(exc))
    if device is None:
        return dict(result, verdict="NO_ADV", detail="扫描窗口未收到目标广播")
    result["mac"] = device.address
    disconnected = asyncio.Event()
    client = BleakClient(device, adapter=args.adapter,
                         disconnected_callback=lambda _: disconnected.set(),
                         timeout=args.connect_timeout)
    task = None
    stamp = time.monotonic()
    phase = "CONNECT"
    counts = {"status": 0, "data": 0}

    def notification(kind):
        def callback(_sender, value):
            counts[kind] += 1
            log.write("NOTIFY", f"{kind} {bytes(value).hex()}")
        return callback

    try:
        if args.load == "frames":
            task = asyncio.create_task(frame_load(console.ser, console, log))
        await asyncio.wait_for(client.connect(), args.connect_timeout + 3)
        result["connect_s"] = round(time.monotonic() - stamp, 3)
        log.write("CONNECTED", f"{device.address} {result['connect_s']}s", echo=True)
        if args.handshake == "sync":
            phase = "GATT"
            status = client.services.get_characteristic(UUID_STATUS)
            data = client.services.get_characteristic(UUID_DATAUP)
            timesync = client.services.get_characteristic(UUID_TIMESYNC)
            if not all((status, data, timesync)):
                return dict(result, verdict="FAIL_GATT", detail="缺少 f1/f2/f3 特征")
            for char, kind in ((status, "status"), (data, "data")):
                log.write("SUBSCRIBE", f"{kind} handle={char.handle}")
                await asyncio.wait_for(client.start_notify(char, notification(kind)),
                                       args.connect_timeout)
            if args.settle:
                await asyncio.sleep(args.settle)
            phase = "SYNC"
            utc = int(time.time())
            payload = struct.pack("<BIh", 1, utc, 480)
            log.write("TIMESYNC", f"utc={utc} payload={payload.hex()}")
            await asyncio.wait_for(client.write_gatt_char(timesync, payload, response=True),
                                   args.connect_timeout)
            deadline = time.monotonic() + args.sync_timeout
            while True:
                fault, detail = link_fault(console, stamp, disconnected, client)
                if fault:
                    return dict(result, verdict=fault, detail=detail)
                if any("BLE: time synced" in line and f"utc={utc}" in line
                       for line in console.since(stamp)):
                    break
                if time.monotonic() >= deadline:
                    return dict(result, verdict="NO_SYNC", detail="未见本轮板端校时确认")
                await asyncio.sleep(0.05)
            result["sync_s"] = round(time.monotonic() - stamp, 3)
            log.write("SYNCED", f"{result['sync_s']}s", echo=True)
        phase = "HOLD"
        hold_start = time.monotonic()
        while time.monotonic() - hold_start < args.hold:
            fault, detail = link_fault(console, stamp, disconnected, client)
            if fault:
                return dict(result, verdict="DIED_HOLD", cause=fault, detail=detail,
                            held_s=round(time.monotonic() - hold_start, 3))
            await asyncio.sleep(0.05)
        fault, detail = link_fault(console, stamp, disconnected, client)
        if fault:
            return dict(result, verdict="DIED_HOLD", cause=fault, detail=detail)
        return dict(result, verdict="SURVIVED", held_s=args.hold,
                    notifications=counts.copy())
    except Exception as exc:
        fault, detail = fault_from_lines(console.since(stamp))
        if console.error:
            fault, detail = "SERIAL_ERROR", console.error
        elif not fault and phase != "CONNECT" and not client.is_connected:
            fault, detail = "FAIL_DROP", "BlueZ disconnected"
        verdict = fault or {"CONNECT": "FAIL_CONN", "GATT": "FAIL_GATT",
                            "SYNC": "FAIL_SYNC", "HOLD": "DIED_HOLD"}[phase]
        return dict(result, verdict=verdict, phase=phase,
                    detail=f"{type(exc).__name__}: {exc}; {detail}")
    finally:
        if task:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        # Mark intentional disconnect in the raw log, outside survival scoring.
        log.write("DISCONNECT_REQUEST", device.address)
        try:
            await asyncio.wait_for(client.disconnect(), 6.0)
        except Exception as exc:
            log.write("DISCONNECT_ERROR", repr(exc))


def check_viewer():
    pidfile = Path("/tmp/aiwatch_viewer.pid")
    if pidfile.exists():
        try:
            os.kill(int(pidfile.read_text().strip()), 0)
        except (ValueError, ProcessLookupError):
            return
        raise RuntimeError("自动投屏守护占用串口；先 systemctl --user stop ai-watch-viewer")


async def run(args):
    import serial

    check_viewer()
    log = RunLog(args.log_prefix)
    ser = serial.Serial(port=None, baudrate=args.baud, timeout=0.05,
                        write_timeout=1, exclusive=True)
    ser.dtr = False
    ser.rts = False
    ser.port = args.port
    console = None
    results = []
    try:
        ser.open()
        console = ConsoleMonitor(ser, log)
        console.start()
        log.write("START", f"port={args.port} adapter={args.adapter} reset=none "
                  f"logs={log.prefix}.*", echo=True)
        if args.startup_wait:
            await asyncio.sleep(args.startup_wait)
        for trial in range(1, args.trials + 1):
            result = await one_trial(args, console, log, trial)
            results.append(result)
            log.write("RESULT", json.dumps(result, ensure_ascii=False), echo=True)
            if trial < args.trials:
                await asyncio.sleep(args.gap)
        tally = Counter(result["verdict"] for result in results)
        log.write("SUMMARY", json.dumps(dict(tally), ensure_ascii=False), echo=True)
        with open(log.prefix + ".results.json", "w", encoding="utf-8") as stream:
            json.dump({"arguments": vars(args), "results": results,
                       "summary": dict(tally)}, stream, ensure_ascii=False, indent=2)
        return 0 if tally["SURVIVED"] == args.trials else 1
    finally:
        if console:
            console.stop_flag.set()
            console.join(timeout=2)
        ser.close()
        log.close()


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=1000000)
    ap.add_argument("-n", "--trials", type=int, default=5)
    ap.add_argument("--adapter", default="hci0")
    ap.add_argument("--load", choices=("none", "frames"), default="none")
    ap.add_argument("--mac", help="只匹配指定地址；默认每轮按广播名/UUID重扫当前随机地址")
    ap.add_argument("--gap", type=float, default=4.0)
    ap.add_argument("--hold", type=float, default=45.0)
    ap.add_argument("--scan-s", type=float, default=15.0)
    ap.add_argument("--connect-timeout", type=float, default=20.0)
    ap.add_argument("--sync-timeout", type=float, default=10.0)
    ap.add_argument("--startup-wait", type=float, default=0.0)
    ap.add_argument("--settle", type=float, default=0.0,
                    help="订阅后、校时前延迟秒数（诊断对照用，默认0）")
    ap.add_argument("--handshake", choices=("sync", "none"), default="sync",
                    help="none 只建链/发现服务并观察；sync 订阅并校时")
    default_log = Path.home() / "huangshan-logs" / datetime.now().strftime("ble_connect_%Y%m%d_%H%M%S")
    ap.add_argument("--log-prefix", default=str(default_log))
    args = ap.parse_args()
    if args.trials <= 0 or min(args.gap, args.hold, args.startup_wait, args.settle) < 0:
        ap.error("次数必须大于0，等待时间不能为负数")
    if min(args.scan_s, args.connect_timeout, args.sync_timeout) <= 0:
        ap.error("扫描/连接/校时超时必须大于0")
    return args


if __name__ == "__main__":
    try:
        raise SystemExit(asyncio.run(run(parse_args())))
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as exc:
        print(f"测试工具失败：{type(exc).__name__}: {exc}", flush=True)
        raise SystemExit(2)
