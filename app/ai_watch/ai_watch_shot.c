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

#include <errno.h>
#include <fcntl.h>
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
 * two bytes of payload, i.e. 3 bytes per pixel. Allocated lazily from
 * the heap (PSRAM is present), never from .bss.
 */

#define SHOT_RLE_BYTES_PER_PX  3

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
static FAR uint8_t      *g_rle_buf;
static size_t            g_rle_cap;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void shot_write_all(FAR const void *data, size_t len)
{
  FAR const uint8_t *p = data;
  size_t off = 0;

  while (off < len)
    {
      ssize_t n = write(STDOUT_FILENO, p + off, len - off);

      if (n <= 0)
        {
          if (n < 0 && errno == EINTR)
            {
              continue;
            }

          break;
        }

      off += (size_t)n;
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

static void shot_dump(FAR lv_display_t *disp, bool compress)
{
  FAR lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
  struct shot_hdr_s hdr;
  uint32_t pixels;
  uint32_t payload_len = 0;
  FAR const uint8_t *payload = NULL;

  if (buf == NULL || buf->data == NULL)
    {
      printf("[SHOT] no active buffer\n");
      return;
    }

  pixels = (uint32_t)buf->header.stride * buf->header.h / 2;

  if (compress)
    {
      size_t need = (size_t)pixels * SHOT_RLE_BYTES_PER_PX;

      if (g_rle_buf == NULL || g_rle_cap < need)
        {
          FAR uint8_t *p = realloc(g_rle_buf, need);

          if (p == NULL)
            {
              printf("[SHOT] RLE buffer alloc failed (%u bytes)\n",
                     (unsigned)need);
              compress = false;
            }
          else
            {
              g_rle_buf = p;
              g_rle_cap = need;
            }
        }

      if (compress)
        {
          payload_len = rle_encode((FAR const uint16_t *)buf->data, pixels,
                                   g_rle_buf, g_rle_cap);

          /* Length 0 means the encoder ran out of room; fall back rather
           * than emit a header with no payload behind it.
           */

          if (payload_len > 0)
            {
              payload = g_rle_buf;
            }
          else
            {
              compress = false;
            }
        }
    }

  if (!compress)
    {
      payload = buf->data;
      payload_len = pixels * 2;
    }

  memcpy(hdr.magic, SHOT_MAGIC, 4);
  hdr.w      = (uint16_t)buf->header.w;
  hdr.h      = (uint16_t)buf->header.h;
  hdr.stride = (uint16_t)buf->header.stride;
  hdr.fmt    = compress ? SHOT_FMT_RLE565 : SHOT_FMT_RGB565;
  hdr.len    = payload_len;

  shot_write_all(&hdr, sizeof(hdr));
  shot_write_all(payload, payload_len);

  printf("\n[SHOT] %ux%u %s %lu bytes\n",
         (unsigned)hdr.w, (unsigned)hdr.h,
         compress ? "rle" : "raw", (unsigned long)payload_len);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ai_watch_shot_init(FAR lv_display_t *disp)
{
  int flags;

  g_shot_disp = disp;

  /* Non-blocking console: the poll below must never park the UI loop
   * waiting for a key that may never come.
   */

  flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (flags >= 0)
    {
      fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

  printf("[SHOT] ready: 's' = frame, 'S' = repeat, 'r' = raw\n");
  return 0;
}

void ai_watch_shot_poll(void)
{
  char ch;

  if (g_shot_disp == NULL)
    {
      return;
    }

  if (read(STDIN_FILENO, &ch, 1) != 1)
    {
      return;
    }

  switch (ch)
    {
      case 's':
        shot_dump(g_shot_disp, true);
        break;

      case 'r':
        shot_dump(g_shot_disp, false);
        break;

      default:
        break;
    }
}
