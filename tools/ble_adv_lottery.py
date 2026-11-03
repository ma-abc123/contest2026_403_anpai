#!/usr/bin/env python3
"""SF32LB52 开机 BLE 广播回归（显式 RTS 复位，不依赖串口 reopen）。

  systemctl --user stop ai-watch-viewer
  /usr/bin/python3 tools/ble_adv_lottery.py -n 8 --scan-s 12

始终只有一个串口读者。每轮 RTS=True 100ms -> False 100ms，DTR 保持
False；对应 sftool 的 SF32LB52 默认硬复位序列：
https://github.com/OpenSiFli/sftool/blob/master/sftool-lib/src/sf32lb52/mod.rs
（attempt_connect；不要发送该函数后续的 Enter 调试命令。）

每轮从新启动日志确认版本和就绪横幅，随后通过 Bleak 的实时广告回调
扫描当前随机地址；不依赖 bluetoothctl 名称输出、不沿用上轮地址。
没有新启动版本横幅时记 RESET_UNCONFIRMED，不把它算作成功的新 boot。
扫描与串口异常独立记为 SCAN_ERROR/SERIAL_ERROR，避免冒充 NO_ADV。

--scan-s 是就绪后扫描窗口，--boot-timeout 是启动横幅的最长等待时间。
保存 .log、逐字节 .raw、带时间/偏移的 .chunks.jsonl 及 .results.json，
格式与 ble_connect_test.py 一致。所有失败轮次保留，不自动丢弃重试。
"""

import argparse
import asyncio
from collections import Counter
from datetime import datetime
import json
from pathlib import Path
import re
import time

from ble_connect_test import ConsoleMonitor, RunLog, check_viewer, find_watch

VERSION = re.compile(r"AI Watch version\s+([^\s]+)")
READY_MARKS = ("[FRM] ready", "UI created; entering main loop")


async def pulse_reset(ser, log):
    ser.dtr = False
    log.write("RESET_ASSERT", "RTS=True DTR=False", echo=True)
    ser.rts = True
    try:
        await asyncio.sleep(0.1)
    finally:
        ser.rts = False
    stamp = time.monotonic()
    log.write("RESET_RELEASE", "RTS=False DTR=False", echo=True)
    await asyncio.sleep(0.1)
    return stamp


async def wait_for_boot(console, stamp, timeout):
    deadline = time.monotonic() + timeout
    while True:
        lines = console.since(stamp)
        version = next((match.group(1) for line in lines
                        if (match := VERSION.search(line))), None)
        ready = any(mark in line for line in lines for mark in READY_MARKS)
        if (version and ready) or console.error or time.monotonic() >= deadline:
            return version, ready
        await asyncio.sleep(0.05)


async def one_boot(args, ser, console, log, boot):
    log.write("BOOT", f"{boot}/{args.boots}", echo=True)
    stamp = await pulse_reset(ser, log)
    version, ready = await wait_for_boot(console, stamp, args.boot_timeout)
    result = {"boot": boot, "version": version, "reset_confirmed": bool(version),
              "ready": ready, "ready_wait_s": round(time.monotonic() - stamp, 3)}
    if console.error:
        return dict(result, verdict="SERIAL_ERROR", detail=console.error)
    if args.send_v:
        ser.write(b"v")
    try:
        device = await find_watch(args, log)
    except Exception as exc:
        return dict(result, verdict="SCAN_ERROR", detail=repr(exc))
    # Recheck after the scan: startup may complete during the scan window.
    lines = console.since(stamp)
    version = next((match.group(1) for line in lines
                    if (match := VERSION.search(line))), version)
    ready = any(mark in line for line in lines for mark in READY_MARKS)
    result.update(version=version, reset_confirmed=bool(version), ready=ready,
                  found=device is not None,
                  mac=getattr(device, "address", None),
                  boot_to_scan_result_s=round(time.monotonic() - stamp, 3),
                  board_adv=any("advertising as" in line for line in lines),
                  hardware_errors=sum("hardware error" in line.lower() for line in lines),
                  controller_recoveries=sum("controller recovery type=" in line for line in lines))
    if console.error:
        return dict(result, verdict="SERIAL_ERROR", detail=console.error)
    if not version:
        return dict(result, verdict="RESET_UNCONFIRMED",
                    detail="RTS 脉冲后未见新的 AI Watch version 横幅")
    return dict(result, verdict="ADV" if device else "NO_ADV")


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
        log.write("START", f"port={args.port} adapter={args.adapter} "
                  f"reset=RTS-100ms logs={log.prefix}.*", echo=True)
        await asyncio.sleep(0.2)  # Drain existing output before the first pulse.
        for boot in range(1, args.boots + 1):
            result = await one_boot(args, ser, console, log, boot)
            results.append(result)
            log.write("RESULT", json.dumps(result, ensure_ascii=False), echo=True)
            if result["verdict"] == "SERIAL_ERROR":
                break
            if boot < args.boots:
                await asyncio.sleep(args.gap)
        tally = Counter(result["verdict"] for result in results)
        log.write("SUMMARY", f"{tally['ADV']}/{args.boots} boots 广播可见；"
                  + json.dumps(dict(tally), ensure_ascii=False), echo=True)
        with open(log.prefix + ".results.json", "w", encoding="utf-8") as stream:
            json.dump({"arguments": vars(args), "results": results,
                       "summary": dict(tally)}, stream, ensure_ascii=False, indent=2)
        return 0 if tally["ADV"] == args.boots else 1
    finally:
        if ser.is_open:
            ser.dtr = False
            ser.rts = False
        if console:
            console.stop_flag.set()
            console.join(timeout=2)
        ser.close()
        log.close()


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=1000000)
    ap.add_argument("-n", "--boots", type=int, default=8)
    ap.add_argument("--adapter", default="hci0")
    ap.add_argument("--mac", help="可选固定地址；默认按名称/服务 UUID 匹配每轮新地址")
    ap.add_argument("--scan-s", type=float, default=12.0)
    ap.add_argument("--boot-timeout", type=float, default=15.0)
    ap.add_argument("--gap", type=float, default=1.0)
    ap.add_argument("--send-v", action="store_true", help="就绪后发送 v 切换串口详细日志")
    ap.add_argument("--keep-open", action="store_true", help="兼容旧参数；现在始终保持串口开启")
    default_log = Path.home() / "huangshan-logs" / datetime.now().strftime("ble_boot_%Y%m%d_%H%M%S")
    ap.add_argument("--log-prefix", default=str(default_log))
    args = ap.parse_args()
    if args.boots <= 0 or min(args.scan_s, args.boot_timeout) <= 0 or args.gap < 0:
        ap.error("次数/超时必须大于0，gap 不能为负数")
    return args


if __name__ == "__main__":
    try:
        raise SystemExit(asyncio.run(run(parse_args())))
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as exc:
        print(f"测试工具失败：{type(exc).__name__}: {exc}", flush=True)
        raise SystemExit(2)
