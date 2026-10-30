/****************************************************************************
 * apps/examples/ai_watch/ai_watch_shot.h
 *
 * Screen dump over the console UART.
 *
 * The LVGL/NuttX driver runs with LV_NUTTX_LCD_BUFFER_COUNT == 1, which
 * makes it allocate a full-screen buffer and render in FULL mode. That
 * buffer always holds the complete frame, so a dump is a plain memcpy of
 * memory we already own -- no interception of putarea() or flush_cb().
 *
 * Trigger: send 's' on the console UART. A frame is ~351 KB at 390x450
 * RGB565, about 2.8 s at 1 Mbaud.
 *
 * Wire format (little endian):
 *   "SHOT" w(2) h(2) stride(2) fmt(1) len(4) then len bytes of pixels.
 * fmt 1 = RGB565. The magic lets the host resynchronise after log lines
 * that interleave with the dump.
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

FAR lv_indev_t *ai_watch_shot_get_indev(void);

#endif /* __APPS_EXAMPLES_AI_WATCH_AI_WATCH_SHOT_H */
