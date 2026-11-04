#!/usr/bin/python3
"""Full-screen style desktop display for a fall-dataset capture run.

Why this exists: the watch's panel is broken until the replacement arrives,
so the only place to read "which action now / how many seconds left / did
that one trigger" is the laptop screen.  tools/capture_monitor.py prints to
a terminal, which is useless when you are standing up doing the actions.

This window shows the same timeline in very large type:

    +-----------------------------------------------------+
    | 静置中        24.7 Hz   样本 812   断流 0           |
    |                                                     |
    |   第 2 / 12 个动作                                   |
    |   站立 -> 侧倒在床垫              应检出             |
    |                                                     |
    |              剩 5 秒                                |
    |   [##########----------]                            |
    |                                                     |
    | 16:32:10  检出！ang=68 deg 3120 mg                  |
    +-----------------------------------------------------+
    | 1 拿起手表 -> 戴上        未检出                    |
    | 2 站立 -> 侧倒在床垫      检出                      |
    +-----------------------------------------------------+

Keys:
    Space   start the timed script (25 s idle, then 8 s per action)
    F       toggle fullscreen
    Esc     quit

The serial stream is still written verbatim to the log file, so the run can
be replayed by tools/capture_analyze.py afterwards.

Usage:
    python3 tools/capture_display.py --log ~/huangshan-logs/cap_run1.log
"""

import argparse
import os
import re
import sys
import threading
import time

try:
    import gi
except ImportError:  # pragma: no cover - wrong interpreter
    sys.exit("需要系统 Python（PyGObject 只装在 /usr/bin/python3）：\n"
             "  /usr/bin/python3 tools/capture_display.py")

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

try:
    import serial
except ImportError:  # pragma: no cover - only when pyserial is missing
    serial = None

REC_RE = re.compile(r"REC,(\d+),")
STEP_RE = re.compile(r"STEP,(\d+),(\d+)")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_script import ACTIONS, action_at, total_s  # noqa: E402

IDLE_S = ACTIONS[0][3] or 25.0

CSS = b"""
window { background-color: #101014; }
.label-dim    { color: #8a8a96; }
.label-body   { color: #e8e8ee; }
.label-tiny   { color: #6a6a76; }
.card         { background-color: #1a1a22; border-radius: 14px; }
.banner-idle  { background-color: #1a1a22; color: #8a8a96; }
.banner-ok    { background-color: #14361f; color: #66d98a; }
.banner-warn  { background-color: #3a2c10; color: #f0b429; }
.banner-hit   { background-color: #4a1013; color: #ff6b6b; }
.banner-miss  { background-color: #4a1013; color: #ff6b6b; }
"""


class Capture(object):
    """Serial reader.  Only touches plain attributes; the GTK thread reads
    them under ``lock`` so no widget is ever touched off the main loop."""

    def __init__(self, port, baud, log_path):
        self.lock = threading.Lock()
        self.err = None
        self.ser = None
        self.log = None
        self.rec_count = 0
        self.last_rec_ms = 0
        self.gaps = []
        self.rec_wall = []        # recent REC arrival times (rate)
        self.events = []          # (wall, kind, text)
        self.steps = []           # (wall, mag) per counted step
        self.stop = False

        try:
            self.log = open(log_path, "w", encoding="utf-8", errors="replace")
        except OSError as exc:
            self.err = "无法写日志 %s: %s" % (log_path, exc)
            return

        if serial is None:
            self.err = "缺少 pyserial（python3 -m pip install pyserial）"
            return

        try:
            self.ser = serial.Serial(port, baud, timeout=0.05)
        except Exception as exc:  # noqa: BLE001 - show any open failure
            self.err = "打不开 %s：%s" % (port, exc)
            return

        # The board's reset circuit is wired to RTS: asserting it holds the
        # SoC in reset (silent UART), so both lines stay low.  A pty has no
        # modem control lines at all, hence the guard (used for dry runs).
        try:
            self.ser.dtr = False
            self.ser.rts = False
        except OSError:
            pass

        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        buf = b""
        while not self.stop:
            try:
                data = self.ser.read(4096)
            except Exception:  # noqa: BLE001 - device unplugged
                break
            if not data:
                continue

            now = time.time()
            if self.log is not None:
                self.log.write(data.decode("utf-8", "replace"))
                self.log.flush()

            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                if not line:
                    continue

                m = REC_RE.search(line)
                if m:
                    t_ms = int(m.group(1))
                    with self.lock:
                        if self.last_rec_ms and t_ms > self.last_rec_ms:
                            gap = t_ms - self.last_rec_ms
                            if gap > 200:
                                self.gaps.append((t_ms, gap))
                        self.last_rec_ms = t_ms
                        self.rec_count += 1
                        self.rec_wall.append(now)
                        if len(self.rec_wall) > 50:
                            self.rec_wall.pop(0)
                    continue

                ms = STEP_RE.search(line)
                if ms:
                    with self.lock:
                        self.steps.append((now, int(ms.group(2))))
                    continue

                kind = None
                if "MOTION: fall armed" in line:
                    kind = "hit"
                elif "MOTION: impact but orientation kept" in line:
                    kind = "reject"
                elif "MOTION: impact followed by motion" in line:
                    kind = "reject"
                elif "MOTION: fall alert cancelled" in line:
                    kind = "cancel"

                if kind is not None:
                    with self.lock:
                        self.events.append((now, kind, line))

    def snapshot(self):
        with self.lock:
            rate = 0.0
            if len(self.rec_wall) >= 2:
                span = self.rec_wall[-1] - self.rec_wall[0]
                if span > 0:
                    rate = (len(self.rec_wall) - 1) / span
            return {
                "rec": self.rec_count,
                "gaps": len(self.gaps),
                "rate": rate,
                "events": list(self.events),
                "steps": list(self.steps),
            }

    def close(self):
        self.stop = True
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:  # noqa: BLE001
                pass
        if self.log is not None:
            try:
                self.log.close()
            except Exception:  # noqa: BLE001
                pass


class Display(object):
    def __init__(self, cap, log_path):
        self.cap = cap
        self.log_path = log_path
        self.t0 = None
        self.hits = {}            # action index -> True if it triggered
        self.seen_events = 0
        self.seen_steps = 0
        self.steps_by_idx = {}    # action index -> counted steps
        self.banner_until = 0.0
        self.last_action = None

        self.win = Gtk.Window(title="跌倒数据采集 — 动作提示")
        self.win.set_default_size(1100, 700)
        self.win.connect("destroy", self.on_destroy)
        self.win.connect("key-press-event", self.on_key)

        style = Gtk.CssProvider()
        style.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_screen(
            Gdk.Screen.get_default(), style,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        root.set_margin_top(16)
        root.set_margin_bottom(16)
        root.set_margin_start(20)
        root.set_margin_end(20)
        self.win.add(root)

        # ---- header: stream health ----
        self.hdr = Gtk.Label()
        self.hdr.set_xalign(0.0)
        self.hdr.set_markup("<span size='18000' foreground='#8a8a96'>"
                            "等待串口…</span>")
        root.pack_start(self.hdr, False, False, 0)

        # ---- current action ----
        mid = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        mid.get_style_context().add_class("card")
        mid.set_border_width(14)

        self.step = Gtk.Label()
        self.step.set_xalign(0.0)
        mid.pack_start(self.step, False, False, 0)

        self.name = Gtk.Label()
        self.name.set_xalign(0.0)
        self.name.set_line_wrap(True)
        mid.pack_start(self.name, False, False, 0)

        self.count = Gtk.Label()
        self.count.set_xalign(0.5)
        mid.pack_start(self.count, False, False, 4)

        self.bar = Gtk.ProgressBar()
        self.bar.set_size_request(-1, 22)
        mid.pack_start(self.bar, False, False, 0)
        root.pack_start(mid, False, False, 0)

        # ---- detection banner ----
        self.banner = Gtk.Label()
        self.banner.set_xalign(0.0)
        self.banner.set_line_wrap(True)
        self.banner.get_style_context().add_class("banner-idle")
        box = Gtk.Box()
        box.get_style_context().add_class("card")
        box.pack_start(self.banner, True, True, 14)
        root.pack_start(box, False, False, 0)
        self.banner_box = box

        # ---- action list ----
        self.listbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3)
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        scroll.add(self.listbox)
        root.pack_start(scroll, True, True, 0)

        self.rows = []
        for i in range(1, len(ACTIONS)):
            row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=14)
            name = Gtk.Label()
            name.set_xalign(0.0)
            name.set_width_chars(24)
            stat = Gtk.Label()
            stat.set_xalign(0.0)
            row.pack_start(name, False, False, 0)
            row.pack_start(stat, False, False, 0)
            self.listbox.pack_start(row, False, False, 0)
            self.rows.append((name, stat))

        self.hint = Gtk.Label()
        self.hint.set_xalign(0.0)
        self.hint.set_markup("<span size='13000' foreground='#6a6a76'>"
                             "空格开始 · F 全屏 · Esc 退出</span>")
        root.pack_start(self.hint, False, False, 0)

        self.render_idle()
        GLib.timeout_add(100, self.tick)

    # ---------- rendering helpers ----------

    @staticmethod
    def set(label, text, size, color="#e8e8ee", bold=False):
        weight = "bold" if bold else "normal"
        label.set_markup(
            "<span size='%d' weight='%s' foreground='%s'>%s</span>"
            % (size, weight, color, GLib.markup_escape_text(text)))

    def row_set(self, i, text, stat, size, color, bold=False):
        name, st = self.rows[i]
        self.set(name, text, size, color, bold)
        self.set(st, stat, size, color, bold)

    def render_idle(self):
        self.set(self.step, "准备", 16000, "#8a8a96")
        self.set(self.name, "按空格开始计时", 36000, "#e8e8ee", True)
        self.set(self.count,
                 "静置 %d 秒 → %d 个动作，共 %d 秒"
                 % (IDLE_S, len(ACTIONS) - 1, int(total_s())), 16000,
                 "#8a8a96")
        self.bar.set_fraction(0.0)
        self.set(self.banner, "尚未开始", 15000, "#8a8a96")
        for i in range(len(self.rows)):
            label, det, steps, _dur = ACTIONS[i + 1]
            mark = "应检出" if det else ("不应检出 · 应 0 步" if steps == 0
                                        else "不应检出")
            self.row_set(i, "%2d  %s" % (i + 1, label), mark, 11000,
                         "#8a8a96")

    def flash(self, text, cls="banner-hit", hold=4.0):
        self.set(self.banner, text, 17000,
                 "#ff6b6b" if cls == "banner-hit" else "#f0b429", True)
        for c in ("banner-idle", "banner-ok", "banner-warn",
                  "banner-hit", "banner-miss"):
            self.banner_box.get_style_context().remove_class(c)
        self.banner_box.get_style_context().add_class(cls)
        self.banner_until = time.time() + hold

    # ---------- main tick ----------

    def tick(self):
        snap = self.cap.snapshot()
        rate = snap["rate"]
        color = "#66d98a" if rate > 20 else "#ff6b6b"

        if self.cap.err:
            self.set(self.hdr, self.cap.err, 14000, "#ff6b6b", True)
            return True

        # new steps since last tick: attribution uses the same timeline as
        # the on-screen prompt, so a slot's tally is exactly what you saw.
        new_steps = snap["steps"][self.seen_steps:]
        self.seen_steps = len(snap["steps"])
        for (wall, _mag) in new_steps:
            sidx = action_at(wall - self.t0)[0] if self.t0 else None
            if sidx is not None:
                self.steps_by_idx[sidx] = self.steps_by_idx.get(sidx, 0) + 1

        # new detections since last tick
        evs = snap["events"][self.seen_events:]
        self.seen_events = len(snap["events"])
        for (wall, kind, line) in evs:
            if self.t0 is None:
                idx = None
            else:
                idx = action_at(wall - self.t0)[0]
            if kind == "hit":
                if idx is not None:
                    self.hits[idx] = True
                self.flash("检出！  " + line.replace("MOTION: ", ""), "banner-hit")
            elif kind == "reject":
                self.flash("冲击但被驳回  " + line.replace("MOTION: ", ""),
                           "banner-warn", hold=3.0)
            elif kind == "cancel":
                self.flash("已取消告警", "banner-warn", hold=3.0)

        hdr = "%s   %.1f Hz   样本 %d   断流 %d   步 %d" % (
            "已开始" if self.t0 else "未开始", rate, snap["rec"],
            snap["gaps"], len(snap["steps"]))
        self.set(self.hdr, hdr, 15000, color, True)

        if self.t0 is None:
            return True

        elapsed = time.time() - self.t0
        idx, label, expect, exp_steps, into, left = action_at(elapsed)
        slot_len = max(into + left, 0.1)

        if idx is None:
            self.set(self.step, "采集结束", 18000, "#66d98a")
            self.set(self.name, "12 个动作已完成，可以 Ctrl-C / Esc 收工",
                     30000, "#e8e8ee", True)
            self.set(self.count, "日志：%s" % self.log_path, 13000, "#8a8a96")
            self.bar.set_fraction(1.0)
            self.refresh_list()
            return True

        if idx == 0:
            self.set(self.step, "静置中（不要动手表）", 18000, "#f0b429")
            self.set(self.name, "25 秒后开始第一个动作", 30000, "#e8e8ee", True)
        else:
            self.set(self.step, "第 %d / %d 个动作" % (idx, len(ACTIONS) - 1),
                     18000, "#8a8a96")
            self.set(self.name, label, 40000, "#e8e8ee", True)

        secs = int(left + 0.999)
        self.set(self.count, "剩 %d 秒" % max(secs, 0), 46000,
                 "#66d98a" if left > 2 else "#ff6b6b", True)
        self.bar.set_fraction(max(0.0, min(1.0, into / slot_len)))

        if self.banner_until < time.time():
            if idx == 0:
                self.set(self.banner, "静置：让设备稳定，别碰手表", 15000,
                         "#8a8a96")
            else:
                self.set(self.banner,
                         "应检出" if expect else "不应检出（误报测试）",
                         15000, "#8a8a96")

        # next-action preview inside the last 2 s of a slot
        if idx > 0 and left <= 2.0 and idx + 1 < len(ACTIONS):
            self.set(self.count, "下一个：%s" % ACTIONS[idx + 1][0], 22000,
                     "#f0b429", True)

        if idx != self.last_action:
            if self.last_action is not None:
                try:
                    Gdk.Display.get_default().beep()
                except Exception:  # noqa: BLE001
                    pass
            self.last_action = idx

        self.refresh_list()
        return True

    def refresh_list(self):
        now = time.time()
        elapsed = (now - self.t0) if self.t0 else 0.0
        cur = action_at(elapsed)[0] if self.t0 else None

        for i in range(len(self.rows)):
            idx = i + 1
            label, expect, exp_steps, _dur = ACTIONS[idx]
            hit = self.hits.get(idx, False)
            got_steps = self.steps_by_idx.get(idx, 0)
            done = cur is not None and cur > idx

            if idx == cur:
                fg, bold = "#ffffff", True
                tail = "← 现在"
            elif not done:
                fg, bold = "#6a6a76", False
                tail = "应检出" if expect else "不应检出"
            else:
                ok = (hit == expect)
                fg = "#66d98a" if ok else "#ff6b6b"
                bold = True
                tail = ("检出" if hit else "未检出") + ("  OK" if ok else "  ✗")

            # Step column: the phantom-step question, answered per action.
            if done or idx == cur:
                if exp_steps == 0:
                    s_ok = got_steps == 0
                    tail += "   步 %d %s" % (got_steps, "OK" if s_ok else "✗")
                else:
                    s_ok = got_steps >= 1
                    tail += "   步 %d %s" % (got_steps, "OK" if s_ok else "✗")
                if not s_ok:
                    fg = "#ff6b6b"
            self.row_set(i, "%2d  %s" % (idx, label), tail, 11000, fg, bold)

    # ---------- events ----------

    def on_key(self, _w, event):
        key = event.keyval
        if key == Gdk.KEY_space and self.t0 is None:
            self.t0 = time.time()
            self.flash("开始：先静置 25 秒", "banner-ok", hold=3.0)
        elif key in (Gdk.KEY_f, Gdk.KEY_F):
            if self.win.get_window().get_state() & Gdk.WindowState.FULLSCREEN:
                self.win.unfullscreen()
            else:
                self.win.fullscreen()
        elif key == Gdk.KEY_Escape:
            self.on_destroy(None)
        return False

    def on_destroy(self, _w):
        self.cap.close()
        Gtk.main_quit()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--log", default=os.path.expanduser(
        "~/huangshan-logs/cap_run1.log"))
    ap.add_argument("--auto-start", action="store_true",
                    help="调试用：窗口打开即开始计时")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.log)), exist_ok=True)

    cap = Capture(args.port, args.baud, args.log)
    disp = Display(cap, args.log)
    if args.auto_start:
        disp.t0 = time.time()

    disp.win.show_all()
    disp.win.maximize()
    try:
        Gtk.main()
    except KeyboardInterrupt:
        cap.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
