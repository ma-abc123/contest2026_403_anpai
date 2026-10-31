#!/usr/bin/env python3
"""Automated self-test for the serial screen-mirroring link.

Two modes:

  python3 tools/shot_selftest.py --unit
      Synthetic tests of the shared frame parser (no board needed):
      RLE/raw round-trips under random chunk splits, console-text
      interleave, magic split across reads, false-magic rejection,
      torn-payload detection.

  python3 tools/shot_selftest.py --live [-p PORT] [--boot-wait S]
      Against a running board:
        1. steady-state frame throughput (back-to-back requests)
        2. a single tap produces exactly one down/up pair
        3. sustained streaming keeps servicing taps (no stall)
        4. tap visibly changes the frame (probe marker or page switch)

Exit code 0 only when every live gate passes (or with --unit, when the
parser tests pass).
"""

import argparse
import io
import random
import struct
import os
import sys
import time

import numpy as np
from PIL import Image

from shot_protocol import FrameParser, build_frame, wait_for_ready

PANEL_W = 390
PANEL_H = 450


# --------------------------------------------------------------------------
# unit mode
# --------------------------------------------------------------------------

def random_image(w, h, seed):
    """RGB565 buffer mixing flat runs (RLE-friendly) and noise (not)."""

    rng = random.Random(seed)
    px = bytearray()
    i = 0
    while i < w * h:
        if rng.random() < 0.7:
            v = rng.randrange(0x10000)
            run = min(rng.randrange(1, 130), w * h - i)
            px += struct.pack("<H", v) * run
            i += run
        else:
            for _ in range(min(20, w * h - i)):
                px += struct.pack("<H", rng.randrange(0x10000))
                i += 1
    return px


def feed_in_chunks(parser, data, max_chunk):
    rng = random.Random(1234)
    off = 0
    img = None
    while off < len(data):
        n = rng.randrange(1, max_chunk + 1)
        out = parser.feed(data[off:off + n])
        if out is not None:
            img = out
        off += n
    return img


def frame_to_rgb565(img, w, h):
    """Inverse of the host RGB888 expansion, for exact comparison."""

    arr = np.asarray(img, dtype=np.uint16)
    r = (arr[:, :, 0] >> 3) << 11
    g = (arr[:, :, 1] >> 2) << 5
    b = (arr[:, :, 2] >> 3)
    return (r | g | b).astype("<u2")


def run_unit():
    checks = []

    for compress, fmt_name in ((True, "rle"), (False, "raw")):
        for w, h in ((195, 225), (390, 450), (64, 64)):
            px = random_image(w, h, seed=w * 1000 + h)
            frame = build_frame(w, h, px, compress=compress)
            sink = io.BytesIO()
            parser = FrameParser(sink)
            img = feed_in_chunks(parser, frame, max_chunk=37)

            ok = img is not None and img.size == (w, h)
            if ok:
                got = frame_to_rgb565(img, w, h).tobytes()
                ok = got == bytes(px)
            checks.append((ok, f"{fmt_name} 往返 {w}x{h} 随机分片"))
            del frame

    # console text before / between frames lands in the sink verbatim
    px = random_image(195, 225, seed=7)
    frame = build_frame(195, 225, px, compress=True)
    stream = (b"MOTION: impact 900 mg (ff=no)\n" + frame +
              b"[FRM] 195x225 rle 100 bytes\n" + frame)
    sink = io.BytesIO()
    parser = FrameParser(sink)
    img = feed_in_chunks(parser, stream, max_chunk=13)
    text = sink.getvalue()
    checks.append((img is not None, "夹在日志里的帧可解码（两次）"))
    checks.append((b"MOTION: impact 900 mg (ff=no)" in text and
                   b"[FRM] 195x225 rle 100 bytes" in text,
                   "控制台文本完整进 sink"))

    # magic split across reads
    sink = io.BytesIO()
    parser = FrameParser(sink)
    parser.feed(b"MOTION: x\nSH")
    img = parser.feed(b"OT" + frame[4:])
    checks.append((img is not None, "跨读的 SHOT 魔数可解析"))
    checks.append((b"MOTION: x\n" in sink.getvalue(), "魔数前文本进 sink"))

    # console line that merely contains the magic must not parse as a frame
    px = random_image(195, 225, seed=8)
    frame = build_frame(195, 225, px, compress=True)
    fake = b"log line SHOT\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\n"
    sink = io.BytesIO()
    parser = FrameParser(sink)
    img = feed_in_chunks(parser, fake + frame, max_chunk=11)
    checks.append((img is not None, "假魔数被拒绝后真帧仍可解析"))
    checks.append((fake in sink.getvalue(), "假魔数原文进 sink"))

    # truncated RLE literal -> payload cannot decode -> failed flag
    px = random_image(195, 225, seed=9)
    frame = build_frame(195, 225, px, compress=True)
    hdr = struct.unpack("<4sHHHBI", frame[:15])
    torn = frame[:15] + frame[15:-3] + b"\xff\xff\xff"
    sink = io.BytesIO()
    parser = FrameParser(sink)
    img = feed_in_chunks(parser, torn, max_chunk=64)
    checks.append((img is None and parser.failed, "撕裂载荷置 failed 且不出图"))

    ok_all = True
    for ok, name in checks:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        ok_all &= ok
    return ok_all


# --------------------------------------------------------------------------
# live mode
# --------------------------------------------------------------------------

class LiveSession:
    def __init__(self, port, baud):
        import serial
        self.ser = serial.Serial(port, baud, timeout=0)
        self.ser.rts = False
        self.ser.dtr = False
        time.sleep(0.3)
        self.ser.reset_input_buffer()
        self.sink = io.BytesIO()
        self.parser = FrameParser(self.sink)
        self.frames = []          # (t, payload)

    def pump(self, duration):
        """Read for `duration` seconds; return frames completed inside."""

        got = []
        t_end = time.time() + duration
        while time.time() < t_end:
            data = self.ser.read(4096)
            if data:
                img = self.parser.feed(data)
                if img is not None:
                    got.append((time.time(), img))
                elif self.parser.failed:
                    got.append((time.time(), None))
            elif time.time() >= t_end:
                break
        self.frames.extend(got)
        return got

    def send(self, data):
        self.ser.write(data)
        self.ser.flush()

    def console(self):
        return self.sink.getvalue()

    def close(self):
        self.ser.close()


def tap_bytes(x, y, down):
    return b"T" + struct.pack("<HHB", x, y, 1 if down else 0)


def drag(sess, x, y0, y1, steps=10, step_ms=0.015, mid_requests=False):
    """Press, sweep to (x, y1) in steps, release. When mid_requests is
    set, a frame is requested per step (stretches the sweep; fine for
    page panning, wrong for timed swipes). Returns frames decoded while
    dragging."""

    frames = []
    sess.send(tap_bytes(x, y0, True))
    for i in range(1, steps + 1):
        time.sleep(step_ms)
        y = y0 + (y1 - y0) * i // steps
        sess.send(tap_bytes(x, int(y), True))
        if mid_requests:
            sess.send(b"s")
            frames += sess.pump(0.08)
    sess.send(tap_bytes(x, y1, False))
    return frames


def grab_frame(sess, deadline=2.0):
    """Request one frame and return it (or None)."""

    sess.send(b"s")
    t_end = time.time() + deadline
    while time.time() < t_end:
        for (_, img) in sess.pump(0.05):
            if img is not None:
                return img
    return None


def run_live(port, baud, boot_wait):
    # The auto-preview daemon (ai-watch-viewer.service) holds the same
    # serial port; concurrent readers split the byte stream, so refuse
    # to run under it rather than fail confusingly mid-test.
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

    print(f"打开 {port} ...")
    sess = LiveSession(port, baud)
    if boot_wait > 0:
        # extra grace on top of the banner wait (legacy knob)
        time.sleep(boot_wait)
    ready, banner = wait_for_ready(sess.ser.read, timeout=15.0)
    if ready:
        print("  板子就绪（启动横幅已出现）")
    else:
        print("  无启动横幅：板子应已在运行（端口打开未触发复位）")
    if b"4.8.4" not in banner:
        print("  警告: 未在启动日志中确认 4.8.4x 版本横幅")
    sess.ser.reset_input_buffer()
    sess.sink.seek(0)
    sess.sink.truncate(0)

    # Pointer packets only trace ([PTR]/[INDEV]) when verbose is on.
    sess.send(b"v")
    time.sleep(0.2)
    sess.ser.reset_input_buffer()
    sess.sink.seek(0)
    sess.sink.truncate(0)

    results = []

    def request_and_collect(n_frames, deadline, tap_every=0.0, tap_xy=None):
        """Host-paced request loop: <=1 outstanding request, like the real
        remote. Optionally sprinkle taps. Returns (frames, taps_sent)."""

        got = []
        taps = 0
        outstanding = False
        last_tap = 0.0
        t0 = time.time()
        while len(got) < n_frames and time.time() - t0 < deadline:
            if not outstanding:
                sess.send(b"s")
                outstanding = True
            new = sess.pump(0.03)
            got += new
            if new:
                # a frame completed (or a torn one was dropped): the
                # request was consumed, send the next one
                outstanding = False
            if tap_every and tap_xy and time.time() - last_tap > tap_every:
                x, y = tap_xy
                sess.send(tap_bytes(x, y, True))
                time.sleep(0.05)
                sess.send(tap_bytes(x, y, False))
                last_tap = time.time()
                taps += 1
        return got, taps

    # 1. steady-state throughput -------------------------------------------
    # Frame sizes ~3.7 KB static / ~15 KB complex at 1 Mbaud wire; BLE init
    # chatter right after boot tears a frame or two, so allow slack.
    t0 = time.time()
    got, _taps = request_and_collect(25, deadline=10)
    span = time.time() - t0
    good = [t for (t, img) in got if img is not None]
    fps = len(good) / span if span > 0 else 0
    results.append((len(good) >= 15 and fps >= 4.0,
                    f"吞吐: 收 {len(good)}/25 帧 {fps:.1f} fps",
                    "≥15 帧且 ≥4 fps（静态表盘）"))

    # 2. swipe up on the watch face opens the app list ----------------------
    # Home-page release handler: dy < -50 px within 300 ms pushes the app
    # list (console signature "Push page 1"). A tight sweep takes the fast
    # path; no frame requests mid-sweep (they would blow the 300 ms).
    sess.ser.reset_input_buffer()
    sess.sink.seek(0)
    sess.sink.truncate(0)
    base = grab_frame(sess)
    drag(sess, x=195, y0=350, y1=150, steps=10, step_ms=0.012)
    sess.pump(1.2)
    text = sess.console().decode("latin-1", "replace")
    pushed = text.count("Push page 1")
    after = grab_frame(sess)
    changed = (base is not None and after is not None and
               np.asarray(base).tobytes() != np.asarray(after).tobytes())
    results.append((pushed >= 1 and changed,
                    f"表盘上滑: Push page×{pushed}，画面{'已变' if changed else '未变'}",
                    "进应用菜单且画面切换"))

    # 3. dragging pans the menu (follows the finger, snaps on release) -----
    # PRESSING handler pans scroll after a 10 px threshold; frames are
    # requested mid-drag to catch the pan in motion.
    sess.ser.reset_input_buffer()
    sess.sink.seek(0)
    sess.sink.truncate(0)
    base = grab_frame(sess)
    mid = drag(sess, x=195, y0=330, y1=110, steps=12, step_ms=0.015,
               mid_requests=True)
    sess.pump(1.0)
    text = sess.console().decode("latin-1", "replace")
    n_motion = text.count("[PTR] 195,")
    mid_changed = any(
        img is not None and base is not None and
        np.asarray(img).tobytes() != np.asarray(base).tobytes()
        for (_, img) in mid)
    time.sleep(2.0)                    # probe marker lingers; let it expire
    settled = grab_frame(sess)
    settled_changed = (base is not None and settled is not None and
                       np.asarray(settled).tobytes() !=
                       np.asarray(base).tobytes())
    results.append((n_motion >= 8 and mid_changed and settled_changed,
                    f"菜单拖拽: 运动包×{n_motion}，拖动中{'跟手' if mid_changed else '无变化'}，"
                    f"吸附后{'移位' if settled_changed else '未移位'}",
                    "≥8 个运动包且拖动中/吸附后画面都变"))

    # 4. exactly-once tap ----------------------------------------------------
    sess.ser.reset_input_buffer()
    sess.sink.seek(0)
    sess.sink.truncate(0)
    sess.send(tap_bytes(30, 30, True))
    time.sleep(0.15)
    sess.send(tap_bytes(30, 30, False))
    sess.pump(1.0)
    text = sess.console().decode("latin-1", "replace")
    n_down = text.count("[PTR] 30,30 down")
    n_up = text.count("[PTR] 30,30 up")
    results.append((n_down == 1 and n_up == 1,
                    f"单击一次: down×{n_down} up×{n_up}", "各恰好 1 次"))

    # 5. streaming keeps servicing taps --------------------------------------
    sess.ser.reset_input_buffer()
    sess.sink.seek(0)
    sess.sink.truncate(0)
    got, taps = request_and_collect(40, deadline=12, tap_every=0.25,
                                    tap_xy=(30, 30))
    text = sess.console().decode("latin-1", "replace")
    n_down = text.count("[PTR] 30,30 down")
    # A tap landing mid-frame has its trace deferred and flushed after the
    # frame, but page-switch log lines printed by the main loop can tear a
    # payload and swallow neighbouring traces. The tap itself is always
    # applied (test 6 proves the UI reacts); allow a couple of casualties.
    results.append((taps >= 5 and taps - n_down <= 2 and len(got) >= 15,
                    f"流式期间连点: 发 {taps} 收 {n_down}，帧 {len(got)}",
                    "至多 2 条痕迹被帧流撕裂，帧流不断"))
    stall = 0.0
    prev = None
    for (t, img) in got:
        if img is None:
            continue
        if prev is not None:
            stall = max(stall, t - prev)
        prev = t
    results.append((stall < 1.0, f"帧间最大空档 {stall:.2f}s", "< 1.0s"))

    # 6. tap changes the frame ------------------------------------------------
    sess.ser.reset_input_buffer()
    sess.send(b"s")
    base = sess.pump(1.5)
    base_img = next((img for (_, img) in reversed(base) if img is not None),
                    None)
    changed = False
    if base_img is not None:
        sess.send(tap_bytes(195, 225, True))
        time.sleep(0.12)
        sess.send(tap_bytes(195, 225, False))
        t_end = time.time() + 3
        while time.time() < t_end:
            for (_, img) in sess.pump(0.05):
                if img is not None and \
                        np.asarray(img).tobytes() != \
                        np.asarray(base_img).tobytes():
                    changed = True
                    break
            if not changed:
                sess.send(b"s")
        results.append((changed, "点击后画面变化", "3s 内出现不同帧"))
    else:
        results.append((False, "点击后画面变化", "基准帧未取到"))

    sess.close()

    ok_all = True
    for ok, name, gate in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}（门槛: {gate}）")
        ok_all &= ok
    return ok_all


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--unit", action="store_true", help="离线解析器测试")
    ap.add_argument("--live", action="store_true", help="实机链路测试")
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=1000000)
    ap.add_argument("--boot-wait", type=float, default=0.0,
                    help=">0 时先等板子启动（刚烧录完用 12）")
    args = ap.parse_args()

    if args.unit:
        print("== unit ==")
        ok = run_unit()
    elif args.live:
        print("== live ==")
        ok = run_live(args.port, args.baud, args.boot_wait)
    else:
        ap.error("选一个模式: --unit 或 --live")

    print("总结: " + ("全部通过" if ok else "存在失败"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
