/****************************************************************************
 * apps/examples/ai_watch/ai_watch_shot.h
 *
 * Screen dump over the console UART. The wire format and the reason no
 * driver interception is needed are documented here.
 *
 * The LVGL/NuttX driver runs with a single full-frame buffer, so a dump
 * is a snapshot of memory we already own -- no interception of putarea()
 * or flush_cb().
 *
 * Trigger: send 's' (RLE) or 'r' (raw RGB565) on the console UART. 'v'
 * toggles per-packet pointer traces.
 *
 * The frame is SNAPSHOTTED into a private buffer, then streamed to the
 * UART in nonblocking chunks: each write() tops up the driver TX ring
 * and returns EAGAIN once it is full, resuming on the next main-loop
 * pass. A frame request that arrives mid-transfer is queued one deep so
 * the host's request/response loop cannot stall.
 *
 * Wire format (little endian):
 *   "SHOT" w(2) h(2) stride(2) fmt(1) len(4) then len bytes of pixels.
 * fmt 1 = RGB565, 2 = RLE565. The magic lets the host resynchronise
 * after log lines that interleave with the dump. Log lines from other
 * threads can still tear an in-flight frame; the host drops those and
 * re-requests on timeout.
 *
 * Touch from the host: 'T' + x(2) + y(2) + state(1), state 1 = down.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_AI_WATCH_AI_WATCH_SHOT_H
#define __APPS_EXAMPLES_AI_WATCH_AI_WATCH_SHOT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <lvgl/lvgl.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ai_watch_shot_init
 *
 * Description:
 *   Remember the display to dump and put the console into non-blocking
 *   mode so polling for the trigger key never stalls the UI loop.
 *
 ****************************************************************************/

int ai_watch_shot_init(FAR lv_display_t *disp);

/****************************************************************************
 * Name: ai_watch_shot_poll
 *
 * Description:
 *   Called once per main-loop iteration. Consumes at most one byte from
 *   the console and dumps a frame when it is 's'.
 *
 ****************************************************************************/

void ai_watch_shot_poll(void);

/****************************************************************************
 * Name: ai_watch_shot_get_indev
 *
 * Description:
 *   The remote pointer registered by ai_watch_shot_init, or NULL. The app
 *   binds this as its touch device when the panel's own touch layer is
 *   unavailable, so gesture handling keeps working off the host mouse.
 *
 ****************************************************************************/

void ai_watch_shot_set_stream_gate(bool block);

FAR lv_indev_t *ai_watch_shot_get_indev(void);

#endif /* __APPS_EXAMPLES_AI_WATCH_AI_WATCH_SHOT_H */
