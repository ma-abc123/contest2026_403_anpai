#!/usr/bin/env python3
"""Live screen preview for the AI Watch over the console UART.

Writes every captured frame to one PNG, meant to be viewed with a window
that reloads on file change:

    terminal 1:  python3 tools/shot_live.py -p /dev/ttyUSB0
    terminal 2:  display -update 1 /tmp/aiwatch_live.png

The watch side compresses each frame with RLE, so a mostly-flat UI costs
a few KB instead of 351 KB. That is what makes this usable for poking at
the touchscreen: move a finger, the picture follows.

The board resets when the port opens (RTS is wired to reset on this
board), so the first frames only appear after it finishes booting.
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial: pip install pyserial")

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("需要 numpy 和 pillow")


MAGIC = b"SHOT"
HDR_LEN = 15                     # 4s + 2H + 2H + 2H + 1B + 4I, packed
FMT_RGB565 = 1
FMT_RLE565 = 2
MAX_PIXELS = 1024 * 1024


def read_exact(ser, n, timeout=5.0):
    buf = bytearray()
    deadline = time.time() + timeout
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if chunk:
            buf.extend(chunk)
            deadline = time.time() + timeout
        elif time.time() > deadline:
            return None
    return bytes(buf)


def find_magic(ser):
    tail = b""
    while True:
        b = ser.read(1)
        if not b:
            return False
        tail = (tail + b)[-4:]
        if tail == MAGIC:
            return True


def rle_decode(data, count):
    """Undo the watch-side RLE: c<0x80 run, c>=0x80 literal block."""
    out = np.empty(count, dtype="<u2")
    o = 0
    i = 0
    n = len(data)

    while i < n and o < count:
        c = data[i]
        i += 1

        if c < 0x80:
            if i + 2 > n:
                break
            run = c + 1
            px = data[i] | (data[i + 1] << 8)
            i += 2
            end = min(o + run, count)
            out[o:end] = px
            o = end
        else:
            lit = (c - 0x7F)
            end = min(o + lit, count)
            need = (end - o) * 2
            if i + need > n:
                break
            chunk = np.frombuffer(data[i:i + need], dtype="<u2")
            out[o:end] = chunk
            i += need
            o = end

    if o < count:
        return None
    return out


def to_rgb565_array(px, w, h, stride):
    """RGB565 -> RGB888 uint8 array, vectorised."""
    r = ((px >> 11) & 0x1F).astype(np.uint8)
    g = ((px >> 5) & 0x3F).astype(np.uint8)
    b = (px & 0x1F).astype(np.uint8)

    return np.dstack([(r << 3) | (r >> 2),
                      (g << 2) | (g >> 4),
                      (b << 3) | (b >> 2)])[:, :w]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=1000000)
    ap.add_argument("-o", "--out", default="/tmp/aiwatch_live.png")
    ap.add_argument("--boot-wait", type=float, default=12.0)
    ap.add_argument("--fps-interval", type=float, default=0.05,
                    help="minimum seconds between frame requests")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    ser.rts = False
    ser.dtr = False
    time.sleep(0.3)
    ser.reset_input_buffer()

    print(f"监听 {args.port} @{args.baud}")
    print(f"等待 {args.boot_wait:.0f}s 启动（打开串口会复位板子）")
    time.sleep(args.boot_wait)
    ser.reset_input_buffer()

    print(f"输出 {args.out}")
    print("另开一个终端运行: display -update 1 " + args.out)
    print("Ctrl-C 结束")

    frames = 0
    t0 = time.time()

    try:
        while True:
            ser.write(b"s")
            ser.flush()

            if not find_magic(ser):
                continue

            body = read_exact(ser, HDR_LEN - 4)
            if body is None:
                continue

            magic, w, h, stride, fmt, length = struct.unpack(
                "<4sHHHBI", MAGIC + body)

            if w == 0 or h == 0 or stride < w * 2 or length > MAX_PIXELS * 2:
                continue

            if length == 0:
                continue

            data = read_exact(ser, length)
            if data is None or len(data) != length:
                continue

            pixels = stride * h // 2

            if fmt == FMT_RLE565:
                px = rle_decode(data, pixels)
            elif fmt == FMT_RGB565:
                px = np.frombuffer(data[:pixels * 2], dtype="<u2")
            else:
                continue

            if px is None:
                continue

            px = px.reshape(h, stride // 2)
            rgb = to_rgb565_array(px, w, h, stride)

            # Write to a temp name then rename: the viewer must never
            # catch a half-written file.
            tmp = args.out + ".tmp"
            Image.fromarray(rgb, "RGB").save(tmp, format="PNG")
            import os
            os.replace(tmp, args.out)

            frames += 1
            if frames % 10 == 0:
                dt = time.time() - t0
                print(f"{frames} 帧, {frames / dt:.1f} fps, "
                      f"本帧 {length} B", end="\r", flush=True)

            time.sleep(args.fps_interval)
    except KeyboardInterrupt:
        print("\n结束")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
