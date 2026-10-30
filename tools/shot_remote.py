#!/usr/bin/env python3
"""Interactive remote screen + pointer for the AI Watch.

With the panel and its touch layer both dead, this window is the only way
to use the watch: it shows the live frame and forwards mouse presses as
touch events over the same UART the frames come back on.

    'T' + x(2 LE) + y(2 LE) + state(1)      state: 1 = down, 0 = up

Coordinates are clamped to the panel, so a drag that runs off the window
edge slides along the border instead of producing nonsense.

Usage:
    python3 tools/shot_remote.py -p /dev/ttyUSB0
"""

import argparse
import os
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial")

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("需要 numpy 和 pillow")

try:
    import Xlib.display
    from Xlib import X
except ImportError:
    sys.exit("需要 python-xlib: pip install --user python-xlib")


MAGIC = b"SHOT"
HDR_LEN = 15
FMT_RGB565 = 1
FMT_RLE565 = 2
MAX_PIXELS = 1024 * 1024

PANEL_W = 390
PANEL_H = 450


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


def find_magic(ser, sink=None):
    tail = b""
    while True:
        b = ser.read(1)
        if not b:
            return False
        tail = (tail + b)[-4:]
        if tail == MAGIC:
            return True
        if sink is not None:
            sink.write(b)


def rle_decode(data, count):
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
            out[o:end] = np.frombuffer(data[i:i + need], dtype="<u2")
            i += need
            o = end

    return out if o == count else None


def grab_frame(ser, sink=None):
    """Request and decode one frame, or return None."""
    ser.write(b"s")
    ser.flush()

    if not find_magic(ser, sink):
        return None

    body = read_exact(ser, HDR_LEN - 4)
    if body is None:
        return None

    magic, w, h, stride, fmt, length = struct.unpack("<4sHHHBI", MAGIC + body)

    if w == 0 or h == 0 or stride < w * 2 or length == 0:
        return None
    if length > MAX_PIXELS * 2:
        return None

    # Sanity bound on the advertised geometry: any console line that
    # happens to contain the magic must not be mistaken for a frame.
    if not (64 <= w <= 1024 and 64 <= h <= 1024):
        return None

    data = read_exact(ser, length)
    if data is None or len(data) != length:
        return None

    pixels = stride * h // 2

    if fmt == FMT_RLE565:
        px = rle_decode(data, pixels)
        if px is None:
            return None
        px = px.reshape(h, stride // 2)
    elif fmt == FMT_RGB565:
        px = np.frombuffer(data[:pixels * 2],
                           dtype="<u2").reshape(h, stride // 2)
    else:
        return None

    px = px[:, :w]
    r = ((px >> 11) & 0x1F).astype(np.uint8)
    g = ((px >> 5) & 0x3F).astype(np.uint8)
    b = (px & 0x1F).astype(np.uint8)

    rgb = np.dstack([(r << 3) | (r >> 2),
                     (g << 2) | (g >> 4),
                     (b << 3) | (b >> 2)])

    return Image.fromarray(rgb, "RGB")


def send_pointer(ser, x, y, down):
    x = max(0, min(PANEL_W - 1, int(x)))
    y = max(0, min(PANEL_H - 1, int(y)))
    n = ser.write(b"T" + struct.pack("<HHB", x, y, 1 if down else 0))
    ser.flush()
    if n != 6:
        print(f"[TX] short write {n}/6 at {x},{y} down={down}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=1000000)
    ap.add_argument("--boot-wait", type=float, default=12.0)
    ap.add_argument("--fps-interval", type=float, default=0.0)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    ser.rts = False
    ser.dtr = False
    time.sleep(0.3)
    ser.reset_input_buffer()

    print(f"等待 {args.boot_wait:.0f}s 让板子启动")
    time.sleep(args.boot_wait)
    ser.reset_input_buffer()

    disp = Xlib.display.Display()
    screen = disp.screen()
    root = screen.root

    win = root.create_window(
        80, 60, PANEL_W, PANEL_H, 0,
        screen.root_depth,
        X.InputOutput,
        X.CopyFromParent,
        background_pixel=screen.black_pixel,
        event_mask=(X.ExposureMask | X.ButtonPressMask | X.ButtonReleaseMask |
                    X.PointerMotionMask | X.StructureNotifyMask),
    )

    # X11 WM_NAME is latin-1; a CJK title would raise on encode.
    win.set_wm_name("AI Watch Remote")
    win.map()
    disp.sync()

    gc = win.create_gc()

    print("窗口已打开：鼠标点击/拖动 = 手表触摸；关窗或 Ctrl-C 退出")

    # Console text interleaved with the frame stream is dropped by the
    # frame parser; keep it so firmware-side traces are still readable.
    log_path = "/tmp/aiwatch_console.log"
    sink = open(log_path, "wb", buffering=0)
    print(f"固件日志写入 {log_path}")

    down = False
    frames = 0
    t0 = time.time()

    try:
        while True:
            # Pump X events first so input feels immediate even while a
            # frame is still being transferred.
            while disp.pending_events():
                e = disp.next_event()

                if e.type == X.ButtonPress:
                    down = True
                    print(f"[X] press {e.event_x},{e.event_y}", flush=True)
                    send_pointer(ser, e.event_x, e.event_y, True)

                elif e.type == X.MotionNotify:
                    if down:
                        send_pointer(ser, e.event_x, e.event_y, True)

                elif e.type == X.ButtonRelease:
                    down = False
                    print(f"[X] release {e.event_x},{e.event_y}", flush=True)
                    send_pointer(ser, e.event_x, e.event_y, False)

                elif e.type == X.Expose:
                    pass

                elif e.type == X.DestroyNotify:
                    raise KeyboardInterrupt

            img = grab_frame(ser, sink)
            if img is not None:
                win.put_pil_image(gc, 0, 0, img)
                disp.flush()
                frames += 1

                # Put the frame counter in the title: the watch face has no
                # moving element while the clock is unsynced, so without
                # this a healthy stream and a frozen one look identical.
                if frames % 5 == 0:
                    try:
                        win.set_wm_name(f"AI Watch Remote - frame {frames}")
                        disp.flush()
                    except Exception:
                        pass

                if frames % 10 == 0:
                    dt = time.time() - t0
                    print(f"{frames} 帧, {frames / dt:.1f} fps   ",
                          end="\r", flush=True)

            if args.fps_interval:
                time.sleep(args.fps_interval)
    except KeyboardInterrupt:
        print("\n退出")
    finally:
        try:
            ser.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
