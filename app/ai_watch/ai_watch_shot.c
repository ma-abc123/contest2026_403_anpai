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
#include <string.h>
#include <unistd.h>

#include "ai_watch_shot.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SHOT_MAGIC          "SHOT"
#define SHOT_FMT_RGB565     1

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

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void shot_dump(FAR lv_display_t *disp)
{
  FAR lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
  struct shot_hdr_s hdr;
  uint32_t len;
  ssize_t written;
  size_t off;

  if (buf == NULL || buf->data == NULL)
    {
      printf("[SHOT] no active buffer\n");
      return;
    }

  len = (uint32_t)buf->header.stride * buf->header.h;

  memcpy(hdr.magic, SHOT_MAGIC, 4);
  hdr.w      = (uint16_t)buf->header.w;
  hdr.h      = (uint16_t)buf->header.h;
  hdr.stride = (uint16_t)buf->header.stride;
  hdr.fmt    = SHOT_FMT_RGB565;
  hdr.len    = len;

  /* Write straight to the fd: stdio would buffer and, worse, interleave
   * its own flush with the pixel stream.
   */

  written = write(STDOUT_FILENO, &hdr, sizeof(hdr));
  if (written != (ssize_t)sizeof(hdr))
    {
      return;
    }

  off = 0;
  while (off < len)
    {
      written = write(STDOUT_FILENO, buf->data + off, len - off);
      if (written <= 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          break;
        }

      off += (size_t)written;
    }

  printf("\n[SHOT] %ux%u stride=%u %lu bytes\n",
         (unsigned)hdr.w, (unsigned)hdr.h, (unsigned)hdr.stride,
         (unsigned long)len);
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

  printf("[SHOT] ready: send 's' on this UART to dump a frame\n");
  return 0;
}

void ai_watch_shot_poll(void)
{
  char ch;

  if (g_shot_disp == NULL)
    {
      return;
    }

  if (read(STDIN_FILENO, &ch, 1) == 1 && ch == 's')
    {
      shot_dump(g_shot_disp);
    }
}
