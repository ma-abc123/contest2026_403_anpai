#!/usr/bin/env python3
"""BLE 连接可靠性探针（主机侧，行为对齐手机 App）。

用途：固件改动后用它做**可重复**的回归判定，而不是靠"试了几次"的印象。
每次尝试完整走一遍握手：扫描 -> 建链 -> 订阅 f1/f3 通知 -> 写 TimeSync
-> 等第一个 Status 通知 -> 断开。任一步失败即判该次失败。

判读要点：
  pass     = 链接存活到握手完成（说明未在连接期被控制器 assert 打断）
  error    = 建链后立刻断（典型：固件报 Hardware error code 0）
  no-scan  = 扫描窗口内手表不在广播态（重初始化空窗，两侧都会出现）

依赖：bleak（`pip install --user bleak`）；需本机 bluetoothd 与可用 hci0。
注意：运行期间会独占 hci0 的扫描，别和 bluetoothctl scan 同时跑。

用法：
    python3 tools/ble_connect_probe.py [次数] [标签]
"""
import asyncio
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

CHR_STATUS = "12345678-9abc-def0-1234-56789abcdef1"
CHR_TIMESYNC = "12345678-9abc-def0-1234-56789abcdef2"
CHR_DATA = "12345678-9abc-def0-1234-56789abcdef3"

ATTEMPTS = int(sys.argv[1]) if len(sys.argv) > 1 else 6
LABEL = sys.argv[2] if len(sys.argv) > 2 else "run"


async def find_watch(timeout=8.0):
  devices = await BleakScanner.discover(timeout=timeout)
  for d in devices:
    if d.name and "AI-Watch" in d.name:
      return d
  return None


async def one_attempt():
  t0 = time.time()
  dev = await find_watch()
  if dev is None:
    return "no-scan", 0.0, ""

  got_notify = asyncio.Event()

  def on_status(_sender, _data):
    got_notify.set()

  try:
    async with BleakClient(dev, timeout=15.0) as client:
      t_conn = time.time() - t0
      await client.start_notify(CHR_STATUS, on_status)
      await client.start_notify(CHR_DATA, lambda s, d: None)

      payload = struct.pack("<BIh", 1, int(time.time()), 480)
      await client.write_gatt_char(CHR_TIMESYNC, payload, response=True)

      try:
        await asyncio.wait_for(got_notify.wait(), timeout=4.0)
        notify = "notify"
      except asyncio.TimeoutError:
        notify = "no-notify"

      await asyncio.sleep(0.5)
      if not client.is_connected:
        return "dropped", t_conn, notify
      return "pass", t_conn, notify
  except Exception as exc:  # noqa: BLE001 - 原样报告原因
    return "error", time.time() - t0, f"{type(exc).__name__}: {exc}"


async def main():
  print(f"=== {LABEL}: {ATTEMPTS} attempts ===")
  results = []
  for i in range(1, ATTEMPTS + 1):
    status, dt, detail = await one_attempt()
    results.append(status)
    print(f"[{i}] {status:<9} {dt:5.1f}s  {detail}")
    await asyncio.sleep(2.0)

  ok = results.count("pass")
  tally = ", ".join(f"{k}={results.count(k)}" for k in sorted(set(results)))
  print(f"=== {LABEL}: PASS {ok}/{ATTEMPTS}  ({tally}) ===")


asyncio.run(main())
