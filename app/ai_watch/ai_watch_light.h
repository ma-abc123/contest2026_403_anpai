/****************************************************************************
 * apps/ai_watch/ai_watch_light.h
 *
 * Ambient light sensor (LTR-303ALS-01 on I2C1 @0x29) and panel brightness.
 *
 * Brightness is expressed in percent of the panel maximum (0..100), which
 * is the same unit the LCD driver's setpower() accepts, so a value can be
 * passed straight through to /dev/lcd0.
 ****************************************************************************/

#ifndef __APPS_AI_WATCH_LIGHT_H
#define __APPS_AI_WATCH_LIGHT_H

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Name: ai_watch_light_init
 *
 * Description:
 *   Open the sensor bus, identify the LTR-303 and start the auto-brightness
 *   worker (auto mode enabled by default). Safe to call when no sensor is
 *   fitted: it logs and returns an error without registering anything.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int ai_watch_light_init(void);

/****************************************************************************
 * Name: ai_watch_light_present
 *
 * Description:
 *   True when the LTR-303 answered with the expected part/manufacturer IDs.
 *
 ****************************************************************************/

bool ai_watch_light_present(void);

/****************************************************************************
 * Name: ai_watch_light_lux
 *
 * Description:
 *   Last computed illuminance in lux (0 when no sensor is present).
 *
 ****************************************************************************/

uint32_t ai_watch_light_lux(void);

/****************************************************************************
 * Name: ai_watch_light_auto / set_auto
 *
 * Description:
 *   Auto-brightness switch. Turning it off keeps the current level and lets
 *   set_level() own the panel; turning it on re-applies on the next sample.
 *
 ****************************************************************************/

bool ai_watch_light_auto(void);
void ai_watch_light_set_auto(bool on);

/****************************************************************************
 * Name: ai_watch_light_level / set_level
 *
 * Description:
 *   Manual brightness in percent. set_level() also switches auto mode off,
 *   which is what a user dragging the level expects.
 *
 ****************************************************************************/

int  ai_watch_light_level(void);
void ai_watch_light_set_level(int percent);

/****************************************************************************
 * Name: ai_watch_light_poll
 *
 * Description:
 *   Apply a level chosen by the sensor worker. Must be called from the
 *   thread that owns the display (the LVGL main loop): the LCD driver has no
 *   lock, so writing the panel from the worker races a frame flush.
 *
 ****************************************************************************/

void ai_watch_light_poll(void);

#endif /* __APPS_AI_WATCH_LIGHT_H */
