/****************************************************************************
 * apps/ai_watch/ai_watch_icons.h
 *
 * Pre-scaled icon image declarations for AI Watch app menu.
 * Five sizes per app: 160px (focus), 100px, 75px, 50px, 25px.
 * Switching image source replaces lv_img_set_zoom — zero runtime scaling.
 *
 * Licensed under the Apache License, Version 2.0.
 ****************************************************************************/

#ifndef APPS_AI_WATCH_AI_WATCH_ICONS_H
#define APPS_AI_WATCH_AI_WATCH_ICONS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <lvgl/lvgl.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AI_WATCH_ICON_TIERS  5

/****************************************************************************
 * Public Data — per-icon, per-tier image descriptors
 ****************************************************************************/

/* Exercise */

extern const lv_image_dsc_t icon_exercise_t0;  /* 160 px */
extern const lv_image_dsc_t icon_exercise_t1;  /* 100 px */
extern const lv_image_dsc_t icon_exercise_t2;  /*  75 px */
extern const lv_image_dsc_t icon_exercise_t3;  /*  50 px */
extern const lv_image_dsc_t icon_exercise_t4;  /*  25 px */

/* Timer */

extern const lv_image_dsc_t icon_timer_t0;
extern const lv_image_dsc_t icon_timer_t1;
extern const lv_image_dsc_t icon_timer_t2;
extern const lv_image_dsc_t icon_timer_t3;
extern const lv_image_dsc_t icon_timer_t4;

/* Reminder */

extern const lv_image_dsc_t icon_reminder_t0;
extern const lv_image_dsc_t icon_reminder_t1;
extern const lv_image_dsc_t icon_reminder_t2;
extern const lv_image_dsc_t icon_reminder_t3;
extern const lv_image_dsc_t icon_reminder_t4;

/* Settings */

extern const lv_image_dsc_t icon_settings_t0;
extern const lv_image_dsc_t icon_settings_t1;
extern const lv_image_dsc_t icon_settings_t2;
extern const lv_image_dsc_t icon_settings_t3;
extern const lv_image_dsc_t icon_settings_t4;

#endif /* APPS_AI_WATCH_AI_WATCH_ICONS_H */
