#!/usr/bin/env python3
"""Interactive remote screen + pointer for the AI Watch.

With the panel and its touch layer both dead, this window is the only way
to use the watch: it shows the live frame and forwards mouse presses as
touch events over the same UART the frames come back on.

    'T' + x(2 LE) + y(2 LE) + state(1)      state: 1 = down, 0 = up

Coordinates are clamped to the panel, so a drag that runs off the window
edge slides along the border instead of producing nonsense.

The firmware streams a frame in small chunks and keeps servicing touch
while it does, so this end must NOT block on a full frame between mouse
events: the loop selects on both the serial port and the X connection and
feeds an incremental frame parser as bytes arrive.

Usage:
    python3 tools/shot_remote.py -p /dev/ttyUSB0
"""

import argparse
import os
import select
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


from shot_protocol import FrameParser, wait_for_ready

PANEL_W = 390
PANEL_H = 450

# A frame request that goes unanswered for this long (torn frame, lost
# request, screen stuck) is re-issued.
REQUEST_TIMEOUT = 1.5


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=1000000)
    ap.add_argument("--boot-wait", type=float, default=0.0)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0)
    ser.rts = False
    ser.dtr = False
    time.sleep(0.3)
    ser.reset_input_buffer()

    # Opening the port resets the board; wait for the app banner so the
    # first frame request (and any queued touch) is not swallowed by
    # boot. --boot-wait only adds extra grace on top.
    if args.boot_wait > 0:
        print(f"额外等待 {args.boot_wait:.0f}s ...")
        time.sleep(args.boot_wait)
    ready, _banner = wait_for_ready(ser.read, timeout=15.0)
    print("板子就绪" if ready else "未见启动横幅：板子应已在运行")
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
    # python-xlib: the X socket lives under display().socket; select() on
    # it alongside the serial port so clicks are sent while a frame is
    # still streaming in.
    x_fd = disp.display.socket.fileno()
    ser_fd = ser.fileno()
    os.set_blocking(ser_fd, False)

    print("窗口已打开：鼠标点击/拖动 = 手表触摸；关窗或 Ctrl-C 退出")

    # Console text interleaved with the frame stream is dropped by the
    # frame parser; keep it so firmware-side traces are still readable.
    log_path = "/tmp/aiwatch_console.log"
    sink = open(log_path, "wb", buffering=0)
    print(f"固件日志写入 {log_path}")

    parser = FrameParser(sink)

    def send_pointer(x, y, down):
        x = max(0, min(PANEL_W - 1, int(x)))
        y = max(0, min(PANEL_H - 1, int(y)))
        n = ser.write(b"T" + struct.pack("<HHB", x, y, 1 if down else 0))
        if n != 6:
            print(f"[TX] short write {n}/6 at {x},{y} down={down}",
                  flush=True)

    def pump_x():
        """Handle every X event currently queued; return False to exit."""

        while disp.pending_events():
            e = disp.next_event()

            if e.type == X.ButtonPress:
                down[0] = True
                print(f"[X] press {e.event_x},{e.event_y}", flush=True)
                send_pointer(e.event_x, e.event_y, True)

            elif e.type == X.MotionNotify:
                if down[0]:
                    send_pointer(e.event_x, e.event_y, True)

            elif e.type == X.ButtonRelease:
                down[0] = False
                print(f"[X] release {e.event_x},{e.event_y}", flush=True)
                send_pointer(e.event_x, e.event_y, False)

            elif e.type == X.DestroyNotify:
                return False

        return True

    def request_frame():
        ser.write(b"s")
        ser.flush()
        return time.time()

    down = [False]
    outstanding_since = None
    frames = 0
    drops = 0
    t0 = time.time()

    try:
        outstanding_since = request_frame()

        while True:
            # X events first: input must never wait behind a frame.
            if not pump_x():
                break

            r, _, _ = select.select([ser_fd, x_fd], [], [], 0.05)

            if x_fd in r:
                if not pump_x():
                    break

            if ser_fd in r:
                try:
                    data = ser.read(4096)
                except (serial.SerialException, OSError) as err:
                    print(f"\n串口错误: {err}")
                    break

                if data:
                    img = parser.feed(data)
                    if img is not None:
                        outstanding_since = None
                        frames += 1

                        # The watch may send a downscaled frame; stretch it
                        # back to panel size so window coordinates map 1:1
                        # to touch coordinates.
                        if img.size != (PANEL_W, PANEL_H):
                            img = img.resize((PANEL_W, PANEL_H),
                                             Image.NEAREST)

                        win.put_pil_image(gc, 0, 0, img)
                        disp.flush()

                        # Put the frame counter in the title: the watch
                        # face has no moving element while the clock is
                        # unsynced, so without this a healthy stream and a
                        # frozen one look identical.
                        if frames % 5 == 0:
                            try:
                                win.set_wm_name(
                                    f"AI Watch Remote - frame {frames}"
                                    f" (drop {drops})")
                                disp.flush()
                            except Exception:
                                pass

                        if frames % 10 == 0:
                            dt = time.time() - t0
                            print(f"{frames} 帧, {frames / dt:.1f} fps,"
                                  f" 丢 {drops}   ", end="\r", flush=True)

                        outstanding_since = request_frame()

                    elif parser.failed:
                        # A body arrived but would not decode (console log
                        # torn through the payload). The parser has already
                        # resynchronised; ask for the next frame at once.
                        drops += 1
                        outstanding_since = request_frame()

            # A request that never produced a frame gets re-issued so one
            # lost frame cannot stall the preview.
            if (outstanding_since is not None and
                    time.time() - outstanding_since > REQUEST_TIMEOUT):
                drops += 1
                outstanding_since = request_frame()

    except KeyboardInterrupt:
        print("\n退出")
    finally:
        try:
            ser.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
