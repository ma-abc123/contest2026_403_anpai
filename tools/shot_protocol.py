#!/usr/bin/env python3
"""SHOT wire protocol shared by the remote-preview tools.

Device framing (little endian, see app/ai_watch/ai_watch_shot.h):

    "SHOT" w(2) h(2) stride(2) fmt(1) len(4) then len bytes of pixels
    fmt 1 = RGB565, 2 = RLE565 (packed run-length, 1 control byte per run)

The firmware streams a frame in small nonblocking chunks and keeps
servicing touch while it does, so console log lines from other threads can
tear an in-flight frame. FrameParser therefore treats the serial stream as
an opaque byte flow: everything that is not a frame (console text) goes to
a sink file, complete frames come back as PIL images, torn payloads are
dropped and the parser resynchronises on the next magic.

This module must stay importable without X or a serial port so the
synthetic unit tests in shot_selftest.py run anywhere.
"""

import struct
import time

try:
    import numpy as np
except ImportError:  # pragma: no cover - pillow/numpy checked by callers
    np = None

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None


MAGIC = b"SHOT"
HDR_LEN = 15
FMT_RGB565 = 1
FMT_RLE565 = 2
MAX_PIXELS = 1024 * 1024

# Printed once by ai_watch_shot_init() when the app console is up and
# accepting commands. Opening the CH340 port RESETS the board (the
# driver's brief default DTR/RTS assertion at open is enough for the
# reset circuit), so hosts must wait for this banner before trusting
# the link -- bytes sent during boot are silently lost.
READY_MARK = b"[FRM] ready"


def wait_for_ready(read, timeout=15.0, settle=4.0):
    """Wait until the firmware console accepts commands.

    read: callable(n) -> bytes (e.g. ser.read). Returns
    (ready, banner_bytes):

      ready True   READY_MARK observed -- fresh boot completed.
      ready False  no banner. Two sub-cases, distinguishable by the
                   returned bytes: empty -> the line stayed silent for
                   `settle` seconds, so the board was already running
                   without a reset (the banner only prints at boot);
                   non-empty -> boot text flowed but never reached the
                   app within `timeout` (stuck or very slow boot).

    Opening the CH340 port resets the board and the first ~3 s of boot
    are completely silent (XIP bringup), so `settle` must comfortably
    exceed that quiet window. All bytes read are returned so callers
    can also check the version banner.
    """

    collected = bytearray()
    t0 = time.time()
    last_data = time.time()
    while time.time() - t0 < timeout:
        chunk = read(4096)
        if chunk:
            collected.extend(chunk)
            last_data = time.time()
            if READY_MARK in collected:
                return True, bytes(collected)
        elif time.time() - last_data > settle and not collected:
            # silent line and nothing ever arrived: board already up
            return False, bytes(collected)
    return READY_MARK in collected, bytes(collected)


def build_frame(w, h, pixels_rgb565, compress=True):
    """Encode a frame the same way the firmware does (for unit tests).

    pixels_rgb565: bytes/bytearray of w*h little-endian RGB565 values.
    Returns the full wire bytes: header + payload.
    """

    stride = w * 2
    if compress:
        payload = bytearray()
        count = w * h
        i = 0
        px = pixels_rgb565
        while i < count:
            v0 = px[2 * i] | (px[2 * i + 1] << 8)
            run = 1
            while (i + run < count and run < 128 and
                   (px[2 * (i + run)] | (px[2 * (i + run) + 1] << 8)) == v0):
                run += 1
            if run >= 3:
                payload.append(run - 1)
                payload.append(v0 & 0xFF)
                payload.append(v0 >> 8)
                i += run
            else:
                start = i
                length = 0
                while i < count and length < 128:
                    if (i + 2 < count and
                            px[2 * i] == px[2 * (i + 1)] == px[2 * (i + 2)] and
                            px[2 * i + 1] ==
                            px[2 * (i + 1) + 1] == px[2 * (i + 2) + 1]):
                        break
                    i += 1
                    length += 1
                payload.append(0x80 | (length - 1))
                for k in range(start, i):
                    payload.append(px[2 * k])
                    payload.append(px[2 * k + 1])
        fmt = FMT_RLE565
    else:
        payload = bytes(pixels_rgb565)
        fmt = FMT_RGB565

    hdr = struct.pack("<4sHHHBI", MAGIC, w, h, stride, fmt, len(payload))
    return hdr + bytes(payload)


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


class FrameParser:
    """Byte-stream reassembler for the SHOT wire format.

    Feed it whatever the serial port hands back; it returns a decoded
    PIL image whenever a full frame lands, or None otherwise. Attributes
    after a feed:

      failed    True when a body-sized frame arrived but would not decode
                (torn payload); the caller should re-request at once.
      completed True when a frame decoded successfully this feed.

    All non-frame bytes (console text, false magics) are written to the
    sink file so firmware traces stay readable.
    """

    WAIT_MAGIC, WAIT_HDR, WAIT_BODY = range(3)

    def __init__(self, sink=None):
        self.sink = sink
        self.state = self.WAIT_MAGIC
        self.tail = b""
        self.buf = bytearray()
        self.pending = bytearray()
        self.w = self.h = self.stride = self.fmt = 0
        self.length = 0
        self.failed = False
        self.completed = False

    def _sink_write(self, data):
        if self.sink is not None and data:
            self.sink.write(data)

    def feed(self, data):
        """Consume serial bytes; return a completed PIL image or None.

        If several frames complete inside one chunk, the last one wins;
        the firmware never sends two frames back to back, so in practice
        there is at most one.
        """

        self.failed = False
        self.completed = False
        img = None
        for byte in data:
            out = self._feed1(byte)
            if out is not None:
                img = out

        if self.state == self.WAIT_MAGIC:
            self._flush_pending()
        return img

    def _flush_pending(self):
        """Push buffered console bytes to the sink, keeping any trailing
        bytes that could be the start of a magic split across reads.
        """

        data = bytes(self.pending)
        if not data:
            return
        keep = 0
        for k in (3, 2, 1):
            if data.endswith(MAGIC[:k]):
                keep = k
                break
        if keep:
            self._sink_write(data[:-keep])
            self.pending = bytearray(data[-keep:])
        else:
            self._sink_write(data)
            self.pending = bytearray()

    def _feed1(self, b):
        if self.state == self.WAIT_MAGIC:
            self.pending.append(b)
            self.tail = (self.tail + bytes((b,)))[-4:]
            if self.tail == MAGIC:
                # The magic itself belongs to the frame, not the console.
                self._sink_write(bytes(self.pending[:-4]))
                self.pending = bytearray()
                self.state = self.WAIT_HDR
                self.buf = bytearray()
        elif self.state == self.WAIT_HDR:
            self.buf.append(b)
            if len(self.buf) == HDR_LEN - 4:
                magic, w, h, stride, fmt, length = struct.unpack(
                    "<4sHHHBI", MAGIC + bytes(self.buf))
                # Sanity bounds so a console line that happens to contain
                # the magic cannot be mistaken for a frame.
                if (64 <= w <= 1024 and 64 <= h <= 1024 and
                        stride >= w * 2 and 0 < length <= MAX_PIXELS * 2):
                    self.w, self.h = w, h
                    self.stride, self.fmt = stride, fmt
                    self.length = length
                    self.state = self.WAIT_BODY
                    self.buf = bytearray()
                else:
                    # False magic; the header bytes are console text.
                    self._sink_write(MAGIC + bytes(self.buf))
                    self.state = self.WAIT_MAGIC
                    self.tail = b""
                    self.buf = bytearray()
        else:  # WAIT_BODY
            self.buf.append(b)
            if len(self.buf) == self.length:
                # Complete mid-chunk: settle immediately so trailing bytes
                # in this chunk go back to the console path instead of
                # being appended past the payload and silently dropped.
                img = self._decode()
                self.state = self.WAIT_MAGIC
                self.tail = b""
                self.buf = bytearray()
                if img is None:
                    self.failed = True
                else:
                    self.completed = True
                return img
        return None

    def _decode(self):
        data = bytes(self.buf)
        pixels = self.stride * self.h // 2

        if self.fmt == FMT_RLE565:
            px = rle_decode(data, pixels)
            if px is None:
                return None
            px = px.reshape(self.h, self.stride // 2)
        elif self.fmt == FMT_RGB565:
            px = np.frombuffer(data[:pixels * 2],
                               dtype="<u2").reshape(self.h, self.stride // 2)
        else:
            return None

        px = px[:, :self.w]
        r = ((px >> 11) & 0x1F).astype(np.uint8)
        g = ((px >> 5) & 0x3F).astype(np.uint8)
        b = (px & 0x1F).astype(np.uint8)

        rgb = np.dstack([(r << 3) | (r >> 2),
                         (g << 2) | (g >> 4),
                         (b << 3) | (b >> 2)])

        return Image.fromarray(rgb, "RGB")
