#!/usr/bin/env python3
"""Capture screen dumps from the AI Watch console UART.

The watch dumps a full frame when it receives 's' on the console:

    "SHOT" w(2) h(2) stride(2) fmt(1) len(4) + len bytes of RGB565

Console log lines can interleave with the pixel stream, so the reader
resyncs on the magic instead of assuming a clean stream. Anything that
fails the size sanity check is discarded and the scan continues.

Usage:
    python3 tools/shot_receiver.py -p /dev/ttyUSB0 -o shot.png
    python3 tools/shot_receiver.py -p /dev/ttyUSB0 -n 5      # 5 frames
    python3 tools/shot_receiver.py -p /dev/ttyUSB0           # watch forever
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial: pip install pyserial")


MAGIC = b"SHOT"
HDR_LEN = 4 + 2 + 2 + 2 + 1 + 4      # 15, packed
MAX_PIXELS = 1024 * 1024              # sanity bound for a single frame


def read_exact(ser, n):
    """Read n bytes or return None on timeout."""
    buf = bytearray()
    deadline = time.time() + 5.0
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if chunk:
            buf.extend(chunk)
            deadline = time.time() + 5.0
            continue
        if time.time() > deadline:
            return None
    return bytes(buf)


def find_magic(ser):
    """Scan the stream for the magic, keeping a 3-byte tail for straddles."""
    tail = b""
    while True:
        b = ser.read(1)
        if not b:
            return False
        tail = (tail + b)[-4:]
        if tail == MAGIC:
            return True


def decode_frame(hdr, data):
    """Turn one frame into a PIL image, or return None if it is garbage."""
    magic, w, h, stride, fmt, length = struct.unpack("<4sHHHBI", hdr)

    if magic != MAGIC or fmt != 1:
        return None
    if length != len(data) or length > MAX_PIXELS * 2:
        return None
    if w == 0 or h == 0 or stride < w * 2 or length < stride * h:
        return None

    try:
        import numpy as np
        from PIL import Image
    except ImportError:
        return ("raw", w, h, stride, data)

    # RGB565 LE -> RGB888, vectorised: a Python loop over 175k pixels
    # takes seconds and this runs on every captured frame.
    raw = np.frombuffer(data[:stride * h], dtype="<u2").reshape(h, stride // 2)
    px = raw[:, :w]

    r = ((px >> 11) & 0x1F).astype(np.uint8)
    g = ((px >> 5) & 0x3F).astype(np.uint8)
    b = (px & 0x1F).astype(np.uint8)

    rgb = np.dstack([(r << 3) | (r >> 2),
                     (g << 2) | (g >> 4),
                     (b << 3) | (b >> 2)])

    return ("img", w, h, Image.fromarray(rgb, "RGB"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=1000000)
    ap.add_argument("-o", "--out", default="shot.png")
    ap.add_argument("-n", "--count", type=int, default=1,
                    help="frames to capture; 0 = forever")
    ap.add_argument("-t", "--timeout", type=float, default=30.0,
                    help="give up after this many seconds without a frame")
    ap.add_argument("--boot-wait", type=float, default=12.0,
                    help="seconds to wait after opening the port; opening it "
                         "resets the board, so this is boot time")
    ap.add_argument("--no-trigger", action="store_true",
                    help="do not send 's' automatically (use when typing it "
                         "from another terminal)")
    args = ap.parse_args()

    # Opening the port asserts DTR/RTS, and on this board RTS high holds
    # the SoC in reset. Release it immediately so the watch keeps running.
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    ser.rts = False
    ser.dtr = False
    time.sleep(0.3)
    ser.reset_input_buffer()

    print(f"监听 {args.port} @{args.baud}")
    if not args.no_trigger:
        print(f"等待 {args.boot_wait:.0f}s 让板子启动（打开串口会复位它）")
        time.sleep(args.boot_wait)
        ser.reset_input_buffer()
        ser.write(b"s")
        ser.flush()
        print("已发送截图指令 s")

    print("按 Ctrl-C 结束")

    got = 0
    last = time.time()

    try:
        while True:
            if args.count and got >= args.count:
                break
            if time.time() - last > args.timeout:
                print(f"{args.timeout:.0f}s 内没有新帧，退出")
                break

            if not find_magic(ser):
                continue

            hdr = read_exact(ser, HDR_LEN - 4)
            if hdr is None:
                continue

            magic, w, h, stride, fmt, length = struct.unpack("<4sHHHBI",
                                                             MAGIC + hdr)
            if fmt != 1 or length > MAX_PIXELS * 2 or w == 0 or h == 0:
                continue

            data = read_exact(ser, length)
            if data is None:
                print(f"帧数据不完整（{length} 字节），丢弃")
                continue

            res = decode_frame(MAGIC + hdr, data)
            if res is None:
                continue

            got += 1
            if res[0] == "img":
                _, fw, fh, img = res
                path = args.out
                if args.count != 1:
                    path = args.out.replace(".png", f"_{got:03d}.png")
                img.save(path)
                print(f"[{got}] {fw}x{fh} -> {path}")
            else:
                _, fw, fh, stride, raw = res
                path = args.out.replace(".png", ".rgb565")
                open(path, "wb").write(raw)
                print(f"[{got}] {fw}x{fh} stride={stride} -> {path} (无 PIL)")

            last = time.time()
    except KeyboardInterrupt:
        print("\n结束")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
