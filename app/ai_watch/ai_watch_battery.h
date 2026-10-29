/****************************************************************************
 * apps/ai_watch/ai_watch_battery.h
 *
 * Battery voltage / state-of-charge.
 ****************************************************************************/

#ifndef __APPS_AI_WATCH_BATTERY_H
#define __APPS_AI_WATCH_BATTERY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Report clamps. The gauge never reports outside this window because the
 * MCU browns out well before the cell is truly empty.
 */

#define AI_WATCH_BATTERY_MIN_MV   3300
#define AI_WATCH_BATTERY_MAX_MV   4200

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ai_watch_battery_init
 *
 * Description:
 *   Open /dev/adc0 and take one reading so the first screen can already show
 *   a value. Safe to call once at start-up.
 *
 ****************************************************************************/

void ai_watch_battery_init(void);

/****************************************************************************
 * Name: ai_watch_battery_poll
 *
 * Description:
 *   Take a reading if the sampling period has elapsed. Must be called from
 *   the main (LVGL) thread; it never blocks for long and touches no shared
 *   peripheral.
 *
 ****************************************************************************/

void ai_watch_battery_poll(void);

/****************************************************************************
 * Name: ai_watch_battery_mv
 *
 * Description:
 *   Filtered battery voltage in millivolts, or -1 when no reading is
 *   available yet.
 *
 ****************************************************************************/

int ai_watch_battery_mv(void);

/****************************************************************************
 * Name: ai_watch_battery_percent
 *
 * Description:
 *   State of charge 0..100, or -1 when unknown.
 *
 ****************************************************************************/

int ai_watch_battery_percent(void);

/****************************************************************************
 * Name: ai_watch_battery_available
 *
 * Description:
 *   True once at least one ADC sample has been converted successfully.
 *
 ****************************************************************************/

bool ai_watch_battery_available(void);

#endif /* __APPS_AI_WATCH_BATTERY_H */
