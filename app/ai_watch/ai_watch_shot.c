/****************************************************************************
 * apps/examples/ai_watch/ai_watch_shot.c
 *
 * Screen dump over the console UART. See ai_watch_shot.h for the wire
 * format and for why no driver interception is needed.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ai_watch_shot.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SHOT_MAGIC          "SHOT"
#define SHOT_FMT_RGB565     1
#define SHOT_FMT_RLE565     2

/* Worst case for the encoder is one literal-control byte per pixel plus
 * two bytes of payload, i.e. 3 bytes per pixel. The frame staging buffer
 * additionally carries the 15-byte header in front of the payload, so a
 * frame is always one contiguous block and the TX state machine has a
 * single offset to track. Allocated lazily from the heap (PSRAM is
 * present), never from .bss.
 */

#define SHOT_RLE_BYTES_PER_PX  3
#define SHOT_HDR_BYTES         15

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Kept byte-aligned and endian-explicit: the host parses it by offset. */

struct shot_hdr_s
{
  char     magic[4];
  uint16_t w;
  uint16_t h;
  uint16_t stride;
  uint8_t  fmt;
  uint32_t len;
} __attribute__((packed));

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR lv_display_t *g_shot_disp;
static FAR lv_indev_t   *g_ptr_indev;
static FAR uint8_t      *g_frame_buf;
static size_t            g_frame_cap;

/* Trace volume: [PTR]/[INDEV] fire on every pointer packet and state edge
 * and flood the console (and the frame stream's bandwidth) during drags.
 * Off by default; toggle with the 'v' key.
 */

static bool              g_verbose;

/* One frame in flight at a time. The frame is snapshotted into
 * g_frame_buf up front, then streamed to the UART in nonblocking chunks:
 * write() tops up the driver TX ring until it returns EAGAIN, and the
 * next main-loop pass resumes. The LVGL loop therefore keeps servicing
 * touch and animations for the whole transfer instead of parking for
 * hundreds of milliseconds per frame -- a full-complexity frame at
 * 1 Mbaud is over a second, which read as "click freezes the watch".
 */

static bool              g_tx_active;
static size_t            g_tx_off;
static size_t            g_tx_len;

/* Stats for the completion line, kept from the last staged frame. */

static uint16_t          g_tx_w;
static uint16_t          g_tx_h;
static uint8_t           g_tx_fmt;
static uint32_t          g_tx_paylen;

/* Stream gate: while set, new frame requests are dropped and any armed
 * frame finishes. The main loop sets it during the BLE connect window
 * (connected but not yet time-synced) so the fragile post-connect period
 * gets the UART and the CPU without a preview stream competing.
 */

static volatile bool g_stream_gate;

/* A frame request that arrives while the previous frame is still going
 * out: remember it (1 deep) so the host's request-response loop cannot
 * deadlock, but do not queue an unbounded backlog.
 */

static bool              g_tx_pending;
static bool              g_tx_pending_compress;

/* Downscale factor for the dump.
 *
 * Costs bytes quadratically (a 2x shrink leaves a quarter of the pixels)
 * and *also* improves the RLE ratio, because block averaging pulls
 * neighbouring pixels closer together. A detailed screen that costs
 * 40 KB at full size lands around 7 KB at 2x.
 *
 * 1 = full resolution, 2 or 3 = decimated. Set from the host with the
 * '1'..'3' keys.
 */

static uint8_t           g_downscale = 2;
static FAR uint16_t     *g_small_buf;
static size_t            g_small_cap;

/* Remote pointer. The panel's own touch layer is dead, so input now
 * arrives from the host over the same UART: 'T' + x(2) + y(2) + state(1),
 * all little endian. A tiny state machine reassembles it because a
 * non-blocking read may hand back the frame in pieces.
 */

#define SHOT_CMD_TOUCH_LEN  6

static uint8_t  g_cmd_buf[SHOT_CMD_TOUCH_LEN];
static uint8_t  g_cmd_len;
static uint32_t g_cmd_start_ms;
static uint16_t g_ptr_x;
static uint16_t g_ptr_y;
static uint8_t  g_ptr_down;

/* A partially received pointer frame must not wedge the parser: if the
 * remaining bytes never arrive, drop what we have and resync on the next
 * command character.
 */

#define SHOT_CMD_TIMEOUT_MS 400

/* Verbose traces printed while a frame is going out would land in the TX
 * ring between payload chunks and tear the frame on the wire. Park them
 * in a small ring and flush after the frame completes; on overflow the
 * oldest line is dropped (traces are diagnostics, frames are product).
 */

#define SHOT_TRACE_LINE   48
#define SHOT_TRACE_QLEN   16

static char    g_trace_q[SHOT_TRACE_QLEN][SHOT_TRACE_LINE];
static uint8_t g_trace_q_head;
static uint8_t g_trace_q_tail;

static void shot_trace_flush(void)
{
  while (g_trace_q_tail != g_trace_q_head)
    {
      printf("%s", g_trace_q[g_trace_q_tail]);
      g_trace_q_tail = (g_trace_q_tail + 1u) % SHOT_TRACE_QLEN;
    }
}

static void shot_trace(FAR const char *fmt, ...)
{
  va_list ap;
  uint8_t next;

  if (!g_verbose)
    {
      return;
    }

  if (g_tx_active)
    {
      next = (g_trace_q_head + 1u) % SHOT_TRACE_QLEN;

      va_start(ap, fmt);
      vsnprintf(g_trace_q[g_trace_q_head], SHOT_TRACE_LINE, fmt, ap);
      va_end(ap);

      if (next == g_trace_q_tail)
        {
          /* full: overwrite the oldest */
          g_trace_q_tail = (g_trace_q_tail + 1u) % SHOT_TRACE_QLEN;
        }

      g_trace_q_head = next;
      return;
    }

  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

/* LVGL samples input every few tens of ms, while a fast click can be down
 * and up again inside one sample. Hold the press briefly so short taps
 * are still seen.
 */

#define SHOT_MIN_PRESS_MS   90

static uint32_t g_ptr_down_ms;
static uint32_t g_ptr_release_ms;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void shot_ptr_read(FAR lv_indev_t *indev, FAR lv_indev_data_t *data)
{
  uint32_t now = lv_tick_get();

  /* Honour a pending short-press hold before reporting the release. */

  if (g_ptr_down && g_ptr_release_ms != 0 &&
      (int32_t)(now - g_ptr_release_ms) >= 0)
    {
      g_ptr_down = 0;
      g_ptr_release_ms = 0;
    }

  LV_UNUSED(indev);

  data->point.x = (lv_coord_t)g_ptr_x;
  data->point.y = (lv_coord_t)g_ptr_y;
  data->state = g_ptr_down ? LV_INDEV_STATE_PRESSED
                           : LV_INDEV_STATE_RELEASED;

  /* Trace on state edges only, and only when verbose; mid-frame the
   * line is parked and flushed after the frame completes.
   */

  if (g_verbose)
    {
      static uint8_t prev_state;

      if ((uint8_t)data->state != prev_state)
        {
          prev_state = (uint8_t)data->state;
          shot_trace("[INDEV] state=%d at %u,%u\n", (int)data->state,
                     (unsigned)g_ptr_x, (unsigned)g_ptr_y);
        }
    }
}

static void shot_ptr_apply(FAR const uint8_t *buf)
{
  uint32_t now = lv_tick_get();
  uint8_t down = buf[5];

  g_ptr_x = (uint16_t)(buf[1] | (buf[2] << 8));
  g_ptr_y = (uint16_t)(buf[3] | (buf[4] << 8));

  if (down)
    {
      g_ptr_down_ms = now;
      g_ptr_release_ms = 0;
      g_ptr_down = 1;
    }
  else if (g_ptr_down)
    {
      uint32_t held = now - g_ptr_down_ms;

      if (held < SHOT_MIN_PRESS_MS)
        {
          g_ptr_release_ms = g_ptr_down_ms + SHOT_MIN_PRESS_MS;
        }
      else
        {
          g_ptr_down = 0;
          g_ptr_release_ms = 0;
        }
    }
  else
    {
      g_ptr_x = (uint16_t)(buf[1] | (buf[2] << 8));
      g_ptr_y = (uint16_t)(buf[3] | (buf[4] << 8));
    }

  if (g_verbose)
    {
      shot_trace("[PTR] %u,%u %s\n", (unsigned)g_ptr_x, (unsigned)g_ptr_y,
                 down ? "down" : "up");
    }
}

/* Packed run-length encoding over RGB565 pixels.
 *
 *   c <  0x80 : a run of (c + 1) copies of the following 2-byte pixel
 *   c >= 0x80 : a literal block of (c - 0x7f) pixels, 2 bytes each
 *
 * A watch face is mostly flat colour, so this turns 351 KB into a few KB
 * -- the difference between "3 s per frame" and a usable preview.
 *
 * Encoding to a private buffer (rather than streaming) is deliberate: the
 * payload length has to be known before the header goes out, and a flush
 * mid-encode would emit a header claiming zero bytes.
 */

static uint32_t rle_encode(FAR const uint16_t *src, uint32_t count,
                           FAR uint8_t *dst, size_t cap)
{
  uint32_t i = 0;
  uint32_t o = 0;

  while (i < count)
    {
      uint16_t v = src[i];
      uint32_t run = 1;

      while (i + run < count && src[i + run] == v && run < 128)
        {
          run++;
        }

      if (run >= 3)
        {
          if (o + 3 > cap)
            {
              return 0;
            }

          dst[o++] = (uint8_t)(run - 1);
          dst[o++] = (uint8_t)(v & 0xff);
          dst[o++] = (uint8_t)(v >> 8);
          i += run;
        }
      else
        {
          uint32_t start = i;
          uint32_t len = 0;

          /* Gather literals, stopping where a run of three begins */

          while (i < count && len < 128)
            {
              if (i + 2 < count && src[i] == src[i + 1] &&
                  src[i] == src[i + 2])
                {
                  break;
                }

              i++;
              len++;
            }

          if (o + 1 + len * 2 > cap)
            {
              return 0;
            }

          dst[o++] = (uint8_t)(0x80 | (len - 1));
          while (start < i)
            {
              uint16_t px = src[start++];

              dst[o++] = (uint8_t)(px & 0xff);
              dst[o++] = (uint8_t)(px >> 8);
            }
        }
    }

  return o;
}

/* Box-filter downscale by an integer factor. Averaging rather than point
 * sampling keeps thin strokes readable at 2x and stops icons from
 * shimmering as the screen changes.
 */

static void shot_downscale(FAR const uint16_t *src, uint16_t w, uint16_t h,
                           uint16_t stride_px, uint8_t scale,
                           FAR uint16_t *dst, uint16_t *out_w,
                           uint16_t *out_h)
{
  uint16_t sw = w / scale;
  uint16_t sh = h / scale;
  uint32_t n = (uint32_t)scale * scale;
  uint16_t x;
  uint16_t y;

  for (y = 0; y < sh; y++)
    {
      FAR const uint16_t *row = src + (uint32_t)(y * scale) * stride_px;

      for (x = 0; x < sw; x++)
        {
          uint32_t r = 0;
          uint32_t g = 0;
          uint32_t b = 0;
          uint16_t dy;
          uint16_t dx;

          for (dy = 0; dy < scale; dy++)
            {
              FAR const uint16_t *py = row + (uint32_t)dy * stride_px;

              for (dx = 0; dx < scale; dx++)
                {
                  uint16_t px = py[(uint32_t)x * scale + dx];

                  r += (px >> 11) & 0x1f;
                  g += (px >> 5) & 0x3f;
                  b += px & 0x1f;
                }
            }

          dst[(uint32_t)y * sw + x] =
            (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }

  *out_w = sw;
  *out_h = sh;
}

/* Snapshot the current LVGL frame into g_frame_buf (header + payload,
 * contiguous) and arm the TX state machine. Encoding happens here,
 * synchronously -- at 2x downscale that is a couple of milliseconds --
 * while the UART transfer itself is deferred to shot_tx_pump() so no
 * main-loop pass ever waits on the wire.
 *
 * The snapshot is deliberate: LVGL keeps rendering into its own buffer
 * while the frame streams out, and the payload must not change under us.
 */

static void shot_frame_start(FAR lv_display_t *disp, bool compress)
{
  FAR lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
  struct shot_hdr_s hdr;
  FAR const uint16_t *pixels;
  uint32_t pixel_count;
  uint32_t payload_len = 0;
  FAR uint8_t *payload;
  size_t need;
  uint16_t w;
  uint16_t h;
  uint16_t stride_px;
  uint8_t scale;

  if (g_tx_active || g_stream_gate)
    {
      return;
    }

  if (buf == NULL || buf->data == NULL)
    {
      printf("[FRM] no active buffer\n");
      return;
    }

  pixels     = (FAR const uint16_t *)buf->data;
  w          = (uint16_t)buf->header.w;
  h          = (uint16_t)buf->header.h;
  stride_px  = (uint16_t)(buf->header.stride / 2);
  pixel_count = (uint32_t)stride_px * h;

  /* Apply the requested downscale, refusing to shrink below a legible
   * size (16 px on the short edge).
   */

  for (scale = g_downscale; scale > 1; scale--)
    {
      if ((w / scale) >= 16 && (h / scale) >= 16)
        {
          break;
        }
    }

  if (scale > 1)
    {
      uint16_t sw = w / scale;
      uint16_t sh = h / scale;
      size_t small_need = (size_t)sw * sh * sizeof(uint16_t);

      if (g_small_buf == NULL || g_small_cap < small_need)
        {
          FAR uint16_t *p = realloc(g_small_buf, small_need);

          if (p != NULL)
            {
              g_small_buf = p;
              g_small_cap = small_need;
            }
        }

      if (g_small_buf != NULL && g_small_cap >= small_need)
        {
          shot_downscale(pixels, w, h, stride_px, scale, g_small_buf,
                         &sw, &sh);
          pixels = g_small_buf;
          w = sw;
          h = sh;
          stride_px = sw;
          pixel_count = (uint32_t)sw * sh;
        }
    }

  need = SHOT_HDR_BYTES + (size_t)pixel_count * SHOT_RLE_BYTES_PER_PX;

  if (g_frame_buf == NULL || g_frame_cap < need)
    {
      FAR uint8_t *p = realloc(g_frame_buf, need);

      if (p == NULL)
        {
          printf("[FRM] frame buffer alloc failed (%u bytes)\n",
                 (unsigned)need);
          return;
        }

      g_frame_buf = p;
      g_frame_cap = need;
    }

  payload = g_frame_buf + SHOT_HDR_BYTES;

  if (compress)
    {
      payload_len = rle_encode(pixels, pixel_count, payload,
                               (size_t)pixel_count * SHOT_RLE_BYTES_PER_PX);

      /* Length 0 means the encoder ran out of room; fall back rather
       * than emit a header with no payload behind it.
       */

      if (payload_len == 0)
        {
          compress = false;
        }
    }

  if (!compress)
    {
      payload_len = pixel_count * 2;
      memcpy(payload, pixels, payload_len);
    }

  memcpy(hdr.magic, SHOT_MAGIC, 4);
  hdr.w      = w;
  hdr.h      = h;
  hdr.stride = (uint16_t)(stride_px * 2);
  hdr.fmt    = compress ? SHOT_FMT_RLE565 : SHOT_FMT_RGB565;
  hdr.len    = payload_len;
  memcpy(g_frame_buf, &hdr, SHOT_HDR_BYTES);

  g_tx_w      = w;
  g_tx_h      = h;
  g_tx_fmt    = hdr.fmt;
  g_tx_paylen = payload_len;
  g_tx_off    = 0;
  g_tx_len    = SHOT_HDR_BYTES + payload_len;
  g_tx_active = true;
}

/* Stream one chunk of the armed frame into the UART TX ring. Called once
 * per main-loop pass; returns as soon as the ring is full (EAGAIN) so the
 * pass stays ~microseconds even mid-frame.
 */

static void shot_tx_pump(FAR lv_display_t *disp)
{
  if (!g_tx_active)
    {
      return;
    }

  while (g_tx_active)
    {
      ssize_t n = write(STDOUT_FILENO, g_frame_buf + g_tx_off,
                        g_tx_len - g_tx_off);

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              /* Ring full: the driver drains it from interrupt context;
               * resume on the next pass.
               */

              return;
            }

          /* Terminal error (e.g. unplugged): drop the frame. The host
           * recovers via its request timeout.
           */

          printf("[FRM] write error %d, frame dropped\n", errno);
          g_tx_active = false;
          return;
        }

      g_tx_off += (size_t)n;

      if (g_tx_off >= g_tx_len)
        {
          g_tx_active = false;
          shot_trace_flush();
          printf("[FRM] %ux%u %s %lu bytes\n",
                 (unsigned)g_tx_w, (unsigned)g_tx_h,
                 g_tx_fmt == SHOT_FMT_RLE565 ? "rle" : "raw",
                 (unsigned long)g_tx_paylen);

          if (g_tx_pending)
            {
              g_tx_pending = false;
              shot_frame_start(disp, g_tx_pending_compress);
            }
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ai_watch_shot_init(FAR lv_display_t *disp)
{
  int flags;

  g_shot_disp = disp;

  /* Non-blocking console, both directions: the poll below must never
   * park the UI loop waiting for a key that may never come, and frame
   * writes must top up the TX ring without ever waiting for the wire to
   * drain.
   */

  flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (flags >= 0)
    {
      fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

  flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
  if (flags >= 0)
    {
      fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

  /* Register the remote pointer. With the panel's own touch layer dead
   * this is the only pointing device LVGL will have, so every click in
   * the host preview window reaches the UI through here.
   */

  g_ptr_indev = lv_indev_create();

  if (g_ptr_indev != NULL)
    {
      /* Bind explicitly: lv_indev_read() bails out early when indev->disp
       * is NULL, and lv_indev_create() only fills it from the *default*
       * display. A self-created display need not be the default, in which
       * case the device would be silently parked and never polled.
       */

      lv_indev_set_display(g_ptr_indev, disp);
      lv_indev_set_type(g_ptr_indev, LV_INDEV_TYPE_POINTER);
      lv_indev_set_read_cb(g_ptr_indev, shot_ptr_read);
    }

  printf("[FRM] ready: 's' = frame, 'r' = raw, 'T' = remote pointer, "
         "'v' = verbose\n");
  return 0;
}

void ai_watch_shot_poll(void)
{
  char ch;

  if (g_shot_disp == NULL)
    {
      return;
    }

  /* Push the in-flight frame forward first: whatever the TX ring accepted
   * since the last pass goes out now, and a finished request chain
   * (pending 's') re-arms here. Only then read commands.
   */

  shot_tx_pump(g_shot_disp);

  /* Drain everything the console has, so a burst of pointer moves is not
   * spread over several frames.
   */

  if (g_cmd_len > 0 &&
      (int32_t)(lv_tick_get() - g_cmd_start_ms) > SHOT_CMD_TIMEOUT_MS)
    {
      g_cmd_len = 0;
    }

  while (read(STDIN_FILENO, &ch, 1) == 1)
    {
      if (g_cmd_len > 0)
        {
          g_cmd_buf[g_cmd_len++] = (uint8_t)ch;

          if (g_cmd_len == SHOT_CMD_TOUCH_LEN)
            {
              shot_ptr_apply(g_cmd_buf);
              g_cmd_len = 0;
            }

          continue;
        }

      switch (ch)
        {
          case 's':
          case 'r':
            if (g_tx_active)
              {
                /* One-deep backlog: keeps the host's request loop alive
                 * without letting it pile up frames we cannot send.
                 */

                g_tx_pending = true;
                g_tx_pending_compress = (ch == 's');
              }
            else
              {
                shot_frame_start(g_shot_disp, ch == 's');
              }
            break;

          case 'T':
            g_cmd_buf[0] = 'T';
            g_cmd_len = 1;
            g_cmd_start_ms = lv_tick_get();
            break;

          case '1':
          case '2':
          case '3':
            g_downscale = (uint8_t)(ch - '0');
            printf("[FRM] downscale %ux\n", (unsigned)g_downscale);
            break;

          case 'v':
            g_verbose = !g_verbose;
            printf("[FRM] verbose %d\n", (int)g_verbose);
            break;

          default:
            break;
        }
    }
}

FAR lv_indev_t *ai_watch_shot_get_indev(void)
{
  return g_ptr_indev;
}

void ai_watch_shot_set_stream_gate(bool block)
{
  if (block)
    {
      g_stream_gate = true;
      g_tx_pending = false;
    }
  else
    {
      g_stream_gate = false;
    }
}
