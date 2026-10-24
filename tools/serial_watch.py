#!/usr/bin/env python3
"""串口守护捕获（板子安全版）。

黄山派复位电路受 CH340 RTS 线电平控制：RTS 拉起 = 芯片被摁在复位（黑屏+串口沉默）。
pyserial 打开串口默认拉起 DTR/RTS，所以打开后必须立刻释放两根线再读。
设备消失自动等待重现并续录，行首打高精度时间戳。
停止: pkill -f serial_watch.py
日志: ~/huangshan-logs/live.log
"""
import serial, time, os

LOG = os.path.expanduser("~/huangshan-logs/live.log")
PORT = "/dev/ttyUSB0"

while True:
    try:
        with serial.Serial(PORT, 1000000, timeout=0.2) as ser:
            ser.dtr = False
            ser.rts = False
            with open(LOG, "a") as out:
                out.write(f"=== REOPEN {time.strftime('%H:%M:%S')} (dtr/rts released) ===\n")
                out.flush()
                while True:
                    data = ser.read(4096)
                    if data:
                        ts = f"[{time.time():.3f}] "
                        out.write(ts + data.decode("utf-8", "replace").replace("\n", "\n" + ts))
                        out.flush()
    except (serial.SerialException, OSError):
        time.sleep(0.5)
