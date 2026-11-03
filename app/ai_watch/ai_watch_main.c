/****************************************************************************
 * apps/ai_watch/ai_watch_main.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <nuttx/lcd/lcd_dev.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/input/buttons.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_touchscreen.h>

#include "ai_watch_icons.h"
#include "fonts/ai_watch_font_cjk_16.h"
#include "fonts/ai_watch_font_weather_28.h"
#include "ai_watch_ble.h"
#include "ai_watch_motion.h"
#include "ai_watch_light.h"
#include "ai_watch_battery.h"
#include "ai_watch_skill.h"
#include "ai_watch_shot.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if AI_WATCH_CAPTURE_STREAM
#  define AI_WATCH_VERSION          "4.8.63-capture"
#else
#  define AI_WATCH_VERSION          "4.8.62-nostore"
#endif
#define AI_WATCH_BUTTON_DEVICE      "/dev/buttons"
#define AI_WATCH_BUTTON_KEY2        (1 << 0)
#define AI_WATCH_BUTTON_POLL_MS     10
#define AI_WATCH_BUTTON_RELEASE_MS  80
#define AI_WATCH_RTC_MIN_YEAR       2020
#define AI_WATCH_RTC_MAX_YEAR       2035
#define AI_WATCH_INPUT_DEVICE       "/dev/input0"

/* Display dimensions */

#define AI_WATCH_SCREEN_WIDTH       390
/* Partial-render strip height (rows): the SRAM render target for the
 * screen-fluidity work. 100 rows = 78 KB, comfortably inside SRAM. */
#define AI_WATCH_LCD_STRIP_ROWS     100
#define AI_WATCH_SCREEN_HEIGHT      450

/* Fixed page IDs */

#define AI_WATCH_PAGE_HOME          0
#define AI_WATCH_PAGE_APP_LIST      1
#define AI_WATCH_PAGE_SETTINGS      2
#define AI_WATCH_PAGE_BRIEF         3
#define AI_WATCH_PAGE_FIXED_COUNT   4

/* App base page ID (dynamic pages start here) */

#define AI_WATCH_PAGE_APP_BASE      100

/* Page stack */

#define AI_WATCH_PAGE_STACK_MAX     8

/* Swipe gesture thresholds */

#define AI_WATCH_SWIPE_THRESHOLD    50
/* 600 ms: the remote virtual pointer (host mouse over UART) adds enough
 * latency that human swipes routinely exceed 300 ms; with the old 300 ms
 * window a remote swipe fell out of the fast path and degraded into a
 * widget click. Panel swipes were never this slow. */
#define AI_WATCH_SWIPE_TIMEOUT_MS   600
/* Back-home gesture: fires mid-drag as soon as the horizontal intent is
 * unambiguous; the release-based fallback allows slow, deliberate swipes.
 */
#define AI_WATCH_SWIPE_BACK_TRIGGER     70
#define AI_WATCH_SWIPE_BACK_TIMEOUT_MS  800

/* Theme definitions */

#define AI_WATCH_THEME_DARK         0
#define AI_WATCH_THEME_LIGHT        1
#define AI_WATCH_THEME_BLUE         2
#define AI_WATCH_THEME_COUNT        3

/* Timer update interval */

#define AI_WATCH_TIMER_UPDATE_MS    50

/* Hex menu layout: 1 + 6 + 12 = 19 max icons */

#define AI_WATCH_MENU_MAX_ICONS     19
#define AI_WATCH_MENU_ICON_SIZE     160  /* largest tier size */
#define AI_WATCH_MENU_RING_SPACING  140

/* Snap animation duration */

#define AI_WATCH_MENU_SNAP_MS       250

/* Minimum drag delta (pixels) to trigger a transform refresh */

#define AI_WATCH_MENU_DRAG_THRESH   3

/* Extra alert-banner types used by the fall pipeline (above the BLE
 * command range so they can never collide with AI_WATCH_BLE_CMD_*).
 */

#define AI_WATCH_ALERT_TYPE_FALL       0x81
#define AI_WATCH_ALERT_TYPE_FALL_INFO  0x82
#define AI_WATCH_ALERT_TYPE_SKILL_SIT  0x83
#define AI_WATCH_ALERT_TYPE_SKILL_GOAL 0x84
#define AI_WATCH_ALERT_TYPE_SKILL_BRIEF 0x85

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Forward declaration */

struct ai_watch_s;

/* App registry entry */

typedef void (*ai_app_create_cb_t)(FAR struct ai_watch_s *watch);
typedef void (*ai_app_destroy_cb_t)(FAR struct ai_watch_s *watch);

struct ai_app_desc_s
{
  FAR const char *name;
  FAR const lv_image_dsc_t *icon_tier[AI_WATCH_ICON_TIERS];
  ai_app_create_cb_t create_cb;
  ai_app_destroy_cb_t destroy_cb;
  bool available;
};

/* Hex menu icon runtime data */

struct ai_menu_icon_s
{
  FAR lv_obj_t *img_obj;
  int app_index;
  float base_x;     /* base position (before scroll offset) */
  float base_y;

  /* Cached last-applied LVGL state (to skip redundant calls) */

  int last_x;
  int last_y;
  int last_tier;    /* -1 = hidden, 0-4 = image tier index */
  lv_opa_t last_opa;
};

/* Hex menu context */

struct ai_menu_ctx_s
{
  FAR lv_obj_t *container;
  struct ai_menu_icon_s icons[AI_WATCH_MENU_MAX_ICONS];
  int icon_count;

  /* Scroll offset (updated by drag) */

  float scroll_x;
  float scroll_y;

  /* Drag state */

  bool dragging;
  int drag_start_x;
  int drag_start_y;
  float drag_start_scroll_x;
  float drag_start_scroll_y;

  /* Focus / snap state */

  int focused_index;
  bool snapping;
  float snap_target_x;
  float snap_target_y;

  /* Last rendered scroll position (for drag threshold) */

  float last_render_scroll_x;
  float last_render_scroll_y;
};

/* Timer state */

struct ai_timer_state_s
{
  FAR lv_obj_t *time_label;
  FAR lv_timer_t *update_timer;
  uint32_t start_tick;
  uint32_t elapsed_ms;
  bool running;
};

/* Main watch state */

struct ai_watch_s
{
  /* Display objects - Home page */

  FAR lv_obj_t *home_screen;
  FAR lv_obj_t *home_title_label;
  FAR lv_obj_t *home_hint_label;
  FAR lv_obj_t *time_label;
  FAR lv_obj_t *seconds_label;
  FAR lv_obj_t *date_label;
  FAR lv_obj_t *unread_label;
  FAR lv_obj_t *bt_label;
  FAR lv_obj_t *battery_shell;
  FAR lv_obj_t *battery_fill;
  FAR lv_obj_t *battery_label;
  int          battery_shown_pct;

  /* Display objects - App list page */

  FAR lv_obj_t *app_list_screen;
  struct ai_menu_ctx_s menu;

  /* Display objects - Settings page */

  FAR lv_obj_t *settings_screen;
  FAR lv_obj_t *settings_list;
  FAR lv_obj_t *bt_switch;
  FAR lv_obj_t *theme_seg_bar;
  FAR lv_obj_t *theme_seg[AI_WATCH_THEME_COUNT];
  FAR lv_obj_t *theme_seg_label[AI_WATCH_THEME_COUNT];
  FAR lv_obj_t *about_label;
  FAR lv_obj_t *ble_state_label;
  FAR lv_obj_t *brightness_slider;
  FAR lv_obj_t *brightness_value;
  FAR lv_obj_t *auto_switch;

  /* Display objects - Dynamic app page */

  FAR lv_obj_t *app_page_screen;
  int active_app_index;

  /* Page navigation */

  int current_page;
  FAR lv_obj_t *fixed_pages[AI_WATCH_PAGE_FIXED_COUNT];
  int page_stack[AI_WATCH_PAGE_STACK_MAX];
  int page_stack_top;

  /* Button state */

  int button_fd;
  btn_buttonset_t supported_buttons;
  struct timespec button_raw_since;
  struct timespec next_button_poll;
  bool button_raw_pressed;
  bool button_armed;

  /* Touch/swipe state */

  FAR lv_indev_t *touch_indev;
  bool touch_active;
  bool gesture_handled;         /* back-home fired mid-drag */
  bool gesture_lock;            /* a slider owns the current touch */

  bool touch_available;
  int touch_start_x;
  int touch_start_y;
  int touch_last_y;             /* previous poll sample, brief scroll */
  struct timespec touch_start_time;

  /* Touch polling */

  struct timespec touch_next_poll;
  int touch_poll_remaining;

  /* Settings page auto-brightness refresh */

  struct timespec next_bright_poll;

  /* RTC state */

  time_t displayed_second;
  bool rtc_valid;
  bool rtc_warning_printed;

  /* Application state */

  bool settings_bt_enabled;
  int current_theme;

  /* BLE state is owned by ai_watch_ble.c; poll ai_watch_ble_get_state() */

  /* Timer state */

  struct ai_timer_state_s timer;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Page management */

static void ai_watch_push_page(FAR struct ai_watch_s *watch, int page);
static void ai_watch_pop_page(FAR struct ai_watch_s *watch);
static void ai_watch_go_home(FAR struct ai_watch_s *watch);
static void ai_watch_open_app(FAR struct ai_watch_s *watch, int app_index);

/* Theme / state */

static void ai_watch_update_bt_label(FAR struct ai_watch_s *watch);
static void ai_watch_update_theme(FAR struct ai_watch_s *watch, int theme);

/* Alert banner (defined after the reminder app section) */

static void ai_watch_alert_apply_theme(int theme);

/* Exercise / motion-service app + fall alarm overlay */

static void ai_watch_create_exercise_app(FAR struct ai_watch_s *watch);
static void ai_watch_destroy_exercise_app(FAR struct ai_watch_s *watch);
static void ai_watch_fall_poll(FAR struct ai_watch_s *watch);

/* UI creation */

static void ai_watch_create_home_page(FAR struct ai_watch_s *watch);
static void ai_watch_create_app_list_page(FAR struct ai_watch_s *watch);
static void ai_watch_create_settings_page(FAR struct ai_watch_s *watch);

/* Timer app */

static void ai_watch_create_timer_app(FAR struct ai_watch_s *watch);
static void ai_watch_destroy_timer_app(FAR struct ai_watch_s *watch);
static void ai_watch_timer_update_cb(FAR lv_timer_t *timer);

/* Reminder app */

static void ai_watch_create_reminder_app(FAR struct ai_watch_s *watch);
static void ai_watch_destroy_reminder_app(FAR struct ai_watch_s *watch);

/* AI assistant app (voice trigger + AI result + countdown) */

static void ai_watch_create_ai_app(FAR struct ai_watch_s *watch);
static void ai_watch_destroy_ai_app(FAR struct ai_watch_s *watch);
static void ai_watch_ai_countdown_poll(FAR struct ai_watch_s *watch);

/* Time */

static void ai_watch_time_update(FAR struct ai_watch_s *watch);

/* Hex menu internals */

static void ai_menu_update_transform(FAR struct ai_watch_s *watch);
static int ai_menu_find_nearest(FAR struct ai_watch_s *watch);
static void ai_menu_start_snap(FAR struct ai_watch_s *watch, int index);
static void ai_menu_event_cb(lv_event_t *e);

/****************************************************************************
 * Private Data - App Registry
 ****************************************************************************/

static const struct ai_app_desc_s g_app_registry[] =
{
  {
    "AI",
    { &icon_ai_t0, &icon_ai_t1, &icon_ai_t2,
      &icon_ai_t3, &icon_ai_t4 },
    ai_watch_create_ai_app, ai_watch_destroy_ai_app, true
  },
  {
    "Timer",
    { &icon_timer_t0, &icon_timer_t1, &icon_timer_t2,
      &icon_timer_t3, &icon_timer_t4 },
    ai_watch_create_timer_app, ai_watch_destroy_timer_app, true
  },
  {
    "Reminder",
    { &icon_reminder_t0, &icon_reminder_t1, &icon_reminder_t2,
      &icon_reminder_t3, &icon_reminder_t4 },
    ai_watch_create_reminder_app, ai_watch_destroy_reminder_app, true
  },
  {
    "Settings",
    { &icon_settings_t0, &icon_settings_t1, &icon_settings_t2,
      &icon_settings_t3, &icon_settings_t4 },
    NULL, NULL, true
  },
  {
    "Exercise",
    { &icon_exercise_t0, &icon_exercise_t1, &icon_exercise_t2,
      &icon_exercise_t3, &icon_exercise_t4 },
    ai_watch_create_exercise_app, ai_watch_destroy_exercise_app, true
  },
};

#define AI_WATCH_APP_COUNT \
  (sizeof(g_app_registry) / sizeof(g_app_registry[0]))

/****************************************************************************
 * Private Functions - Utilities
 ****************************************************************************/

static int64_t ai_watch_elapsed_ms(FAR const struct timespec *now,
                                   FAR const struct timespec *then)
{
  return (int64_t)(now->tv_sec - then->tv_sec) * 1000 +
         (now->tv_nsec - then->tv_nsec) / 1000000;
}

static bool ai_watch_deadline_reached(FAR const struct timespec *now,
                                      FAR const struct timespec *deadline)
{
  return now->tv_sec > deadline->tv_sec ||
         (now->tv_sec == deadline->tv_sec &&
          now->tv_nsec >= deadline->tv_nsec);
}

static void ai_watch_advance_deadline(FAR struct timespec *deadline,
                                      long delay_ms)
{
  deadline->tv_nsec += delay_ms * 1000000;
  deadline->tv_sec += deadline->tv_nsec / 1000000000;
  deadline->tv_nsec %= 1000000000;
}

static int ai_watch_day_of_week(int year, int month, int day)
{
  static const int t[] =
  {
    0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4
  };

  if (month < 3)
    {
      year--;
    }

  return (year + year / 4 - year / 100 + year / 400 +
          t[month - 1] + day) % 7;
}

/****************************************************************************
 * Private Functions - Hex Menu: Coordinate System
 *
 * Hex ring layout: center (0,0), ring 1 has 6 positions, ring 2 has 12.
 * Positions use axial coordinates (col, row) mapped to 60-degree angles.
 *
 *   Ring 0:  1 icon   at (0,0)
 *   Ring 1:  6 icons  at 60-degree intervals
 *   Ring 2:  12 icons at 30-degree intervals
 *   Ring N:  6*N icons
 *
 * The total capacity is 1 + 6 + 12 = 19 icons for 2 rings.
 ****************************************************************************/

static void ai_menu_hex_position(int index, float *out_x, float *out_y)
{
  float r;
  float angle_rad;

  if (index == 0)
    {
      *out_x = 0.0f;
      *out_y = 0.0f;
      return;
    }

  /* Find which ring this index belongs to */

  if (index <= 6)
    {
      /* Ring 1: 6 positions at 60-degree intervals, starting at 90 degrees */

      r = (float)AI_WATCH_MENU_RING_SPACING;
      angle_rad = (float)(index - 1) * (float)M_PI / 3.0f;
      *out_x = r * cosf(angle_rad);
      *out_y = r * sinf(angle_rad);
    }
  else
    {
      /* Ring 2: 12 positions at 30-degree intervals, offset by 15 degrees */

      int idx2 = index - 7;

      r = (float)AI_WATCH_MENU_RING_SPACING * 2.0f;
      angle_rad = (float)idx2 * (float)M_PI / 6.0f;
      *out_x = r * cosf(angle_rad);
      *out_y = r * sinf(angle_rad);
    }
}

/****************************************************************************
 * Private Functions - Hex Menu: Transform & Rendering
 *
 * Pre-scaled icon tiers replace lv_img_set_zoom entirely:
 *   tier 0 = 160px (focused), tier 1 = 100px, tier 2 = 75px,
 *   tier 3 = 50px, tier 4 = 25px, tier -1 = hidden.
 *
 * Distance-squared thresholds (no sqrtf) select the tier.
 * Cached (x, y, tier, opa) per icon; LVGL setters called only on change.
 ****************************************************************************/

/* Tier selection distance-squared thresholds (pixels²).
 * Tier 0: focused (always full opacity)
 * Tier 1: d² < 100²  → 100px, 100% opacity
 * Tier 2: d² < 180²  →  75px,  80% opacity
 * Tier 3: d² < 280²  →  50px,  50% opacity
 * Tier 4: d² < 400²  →  25px,  25% opacity
 * Beyond: hidden
 */

#define MENU_D2_TIER1  (100.0f * 100.0f)
#define MENU_D2_TIER2  (180.0f * 180.0f)
#define MENU_D2_TIER3  (280.0f * 280.0f)
#define MENU_D2_HIDE   (400.0f * 400.0f)

/* Icon pixel sizes per tier (must match generated icon dimensions) */

static const int menu_tier_size[AI_WATCH_ICON_TIERS] =
{
  160, 100, 75, 50, 25
};

/* Opacity per tier (percent of LV_OPA_COVER) */

static const int menu_tier_opa_pct[AI_WATCH_ICON_TIERS] =
{
  100, 100, 80, 50, 25
};

static void ai_menu_update_transform(FAR struct ai_watch_s *watch)
{
  FAR struct ai_menu_ctx_s *ctx = &watch->menu;
  float scr_cx = (float)AI_WATCH_SCREEN_WIDTH / 2.0f;
  float scr_cy = (float)AI_WATCH_SCREEN_HEIGHT / 2.0f;
  int new_focused;
  int i;

  new_focused = ai_menu_find_nearest(watch);

  for (i = 0; i < ctx->icon_count; i++)
    {
      FAR struct ai_menu_icon_s *icon = &ctx->icons[i];
      FAR const struct ai_app_desc_s *app;
      float screen_x;
      float screen_y;
      float dx;
      float dy;
      float d2;
      int tier;
      lv_opa_t opa;
      int ix;
      int iy;
      int half_size;

      if (icon->img_obj == NULL)
        {
          continue;
        }

      /* Icon screen position = base - scroll + center */

      screen_x = icon->base_x - ctx->scroll_x + scr_cx;
      screen_y = icon->base_y - ctx->scroll_y + scr_cy;

      /* Off-screen culling (generous margin for largest tier) */

      if (screen_x < -170.0f ||
          screen_x > (float)AI_WATCH_SCREEN_WIDTH + 170.0f ||
          screen_y < -170.0f ||
          screen_y > (float)AI_WATCH_SCREEN_HEIGHT + 170.0f)
        {
          tier = -1;
        }
      else
        {
          dx = screen_x - scr_cx;
          dy = screen_y - scr_cy;
          d2 = dx * dx + dy * dy;

          if (i == new_focused)
            {
              tier = 0;
            }
          else if (d2 < MENU_D2_TIER1)
            {
              tier = 1;
            }
          else if (d2 < MENU_D2_TIER2)
            {
              tier = 2;
            }
          else if (d2 < MENU_D2_TIER3)
            {
              tier = 3;
            }
          else if (d2 < MENU_D2_HIDE)
            {
              tier = 4;
            }
          else
            {
              tier = -1;
            }
        }

      /* Hide completely */

      if (tier < 0)
        {
          if (icon->last_tier >= 0)
            {
              lv_obj_add_flag(icon->img_obj, LV_OBJ_FLAG_HIDDEN);
              icon->last_tier = -1;
            }

          continue;
        }

      /* Compute opacity */

      opa = (lv_opa_t)(LV_OPA_COVER * menu_tier_opa_pct[tier] / 100);

      /* Pixel position (centered on screen point) */

      half_size = menu_tier_size[tier] / 2;
      ix = (int)(screen_x) - half_size;
      iy = (int)(screen_y) - half_size;

      /* Unhide if was hidden */

      if (icon->last_tier < 0)
        {
          lv_obj_clear_flag(icon->img_obj, LV_OBJ_FLAG_HIDDEN);
        }

      /* Switch image source if tier changed */

      if (tier != icon->last_tier)
        {
          app = &g_app_registry[icon->app_index];
          lv_img_set_src(icon->img_obj, app->icon_tier[tier]);
          icon->last_tier = tier;
        }

      /* Update position if changed */

      if (ix != icon->last_x || iy != icon->last_y)
        {
          lv_obj_set_pos(icon->img_obj, ix, iy);
          icon->last_x = ix;
          icon->last_y = iy;
        }

      /* Update opacity if changed */

      if (opa != icon->last_opa)
        {
          lv_obj_set_style_opa(icon->img_obj, opa, 0);
          icon->last_opa = opa;
        }
    }

  ctx->focused_index = new_focused;
}

static int ai_menu_find_nearest(FAR struct ai_watch_s *watch)
{
  FAR struct ai_menu_ctx_s *ctx = &watch->menu;
  float scr_cx = (float)AI_WATCH_SCREEN_WIDTH / 2.0f;
  float scr_cy = (float)AI_WATCH_SCREEN_HEIGHT / 2.0f;
  float min_dist = 1e30f;
  int nearest = 0;
  int i;

  for (i = 0; i < ctx->icon_count; i++)
    {
      float screen_x;
      float screen_y;
      float dx;
      float dy;
      float dist;

      screen_x = ctx->icons[i].base_x - ctx->scroll_x + scr_cx;
      screen_y = ctx->icons[i].base_y - ctx->scroll_y + scr_cy;
      dx = screen_x - scr_cx;
      dy = screen_y - scr_cy;
      dist = dx * dx + dy * dy;

      if (dist < min_dist)
        {
          min_dist = dist;
          nearest = i;
        }
    }

  return nearest;
}

/****************************************************************************
 * Private Functions - Hex Menu: Snap Animation
 *
 * Two separate exec callbacks:
 *   - ai_menu_snap_exec_x_cb: updates scroll_x only (no transform).
 *   - ai_menu_snap_exec_y_cb: updates scroll_y and calls transform
 *     once per frame.  user_data carries the watch pointer.
 ****************************************************************************/

static void ai_menu_snap_exec_x_cb(void *var, int32_t value)
{
  *(FAR float *)var = (float)value;
}

static void ai_menu_snap_exec_y_cb(void *var, int32_t value)
{
  FAR lv_anim_t *a;

  *(FAR float *)var = (float)value;

  /* Retrieve watch from user_data and refresh transform */

  a = lv_anim_get(var, ai_menu_snap_exec_y_cb);
  if (a != NULL)
    {
      FAR struct ai_watch_s *w = lv_anim_get_user_data(a);

      if (w != NULL)
        {
          ai_menu_update_transform(w);
        }
    }
}

static void ai_menu_snap_ready_cb(lv_anim_t *a)
{
  FAR struct ai_watch_s *w = lv_anim_get_user_data(a);

  if (w != NULL)
    {
      w->menu.snapping = false;
      ai_menu_update_transform(w);
    }
}

static void ai_menu_start_snap(FAR struct ai_watch_s *watch, int index)
{
  FAR struct ai_menu_ctx_s *ctx = &watch->menu;
  float target_x;
  float target_y;
  lv_anim_t a;

  if (index < 0 || index >= ctx->icon_count)
    {
      return;
    }

  target_x = ctx->icons[index].base_x;
  target_y = ctx->icons[index].base_y;

  ctx->snapping = true;

  /* Animate scroll_x — no transform refresh */

  lv_anim_init(&a);
  lv_anim_set_var(&a, &ctx->scroll_x);
  lv_anim_set_values(&a, (int32_t)ctx->scroll_x,
                     (int32_t)target_x);
  lv_anim_set_time(&a, AI_WATCH_MENU_SNAP_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, ai_menu_snap_exec_x_cb);
  lv_anim_start(&a);

  /* Animate scroll_y — exec callback also refreshes transform */

  lv_anim_init(&a);
  lv_anim_set_var(&a, &ctx->scroll_y);
  lv_anim_set_values(&a, (int32_t)ctx->scroll_y,
                     (int32_t)target_y);
  lv_anim_set_time(&a, AI_WATCH_MENU_SNAP_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, ai_menu_snap_exec_y_cb);
  lv_anim_set_user_data(&a, watch);
  lv_anim_set_ready_cb(&a, ai_menu_snap_ready_cb);
  lv_anim_start(&a);

  ai_menu_update_transform(watch);
}

/****************************************************************************
 * Private Functions - Hex Menu: Event Handling
 ****************************************************************************/

static void ai_menu_event_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);
  FAR struct ai_menu_ctx_s *ctx = &watch->menu;
  lv_event_code_t code = lv_event_get_code(e);
  FAR lv_indev_t *indev;
  lv_point_t point;

  indev = lv_indev_get_act();
  if (indev == NULL)
    {
      return;
    }

  lv_indev_get_point(indev, &point);

  if (code == LV_EVENT_PRESSED)
    {
      /* Stop any running snap animation */

      if (ctx->snapping)
        {
          lv_anim_del(&ctx->scroll_x, NULL);
          lv_anim_del(&ctx->scroll_y, NULL);
          ctx->snapping = false;
        }

      ctx->dragging = false;
      ctx->drag_start_x = point.x;
      ctx->drag_start_y = point.y;
      ctx->drag_start_scroll_x = ctx->scroll_x;
      ctx->drag_start_scroll_y = ctx->scroll_y;
    }
  else if (code == LV_EVENT_PRESSING)
    {
      int dx = point.x - ctx->drag_start_x;
      int dy = point.y - ctx->drag_start_y;
      int threshold = 10;
      float new_scroll_x;
      float new_scroll_y;
      float delta_x;
      float delta_y;

      /* Start dragging after threshold */

      if (!ctx->dragging &&
          (abs(dx) > threshold || abs(dy) > threshold))
        {
          ctx->dragging = true;
        }

      if (ctx->dragging)
        {
          new_scroll_x = ctx->drag_start_scroll_x - (float)dx;
          new_scroll_y = ctx->drag_start_scroll_y - (float)dy;

          /* Skip refresh if movement is too small */

          delta_x = new_scroll_x - ctx->last_render_scroll_x;
          delta_y = new_scroll_y - ctx->last_render_scroll_y;

          if (delta_x * delta_x + delta_y * delta_y <
              (float)(AI_WATCH_MENU_DRAG_THRESH *
                      AI_WATCH_MENU_DRAG_THRESH))
            {
              return;
            }

          ctx->scroll_x = new_scroll_x;
          ctx->scroll_y = new_scroll_y;
          ctx->last_render_scroll_x = new_scroll_x;
          ctx->last_render_scroll_y = new_scroll_y;
          ai_menu_update_transform(watch);
        }
    }
  else if (code == LV_EVENT_RELEASED ||
           code == LV_EVENT_PRESS_LOST)
    {
      if (ctx->dragging)
        {
          /* Drag ended: snap to nearest icon */

          int nearest = ai_menu_find_nearest(watch);

          ai_menu_start_snap(watch, nearest);
          ctx->dragging = false;
        }
      else
        {
          /* Tap (no drag): find which icon was tapped */

          float scr_cx = (float)AI_WATCH_SCREEN_WIDTH / 2.0f;
          float scr_cy = (float)AI_WATCH_SCREEN_HEIGHT / 2.0f;
          int i;

          for (i = 0; i < ctx->icon_count; i++)
            {
              float sx = ctx->icons[i].base_x - ctx->scroll_x + scr_cx;
              float sy = ctx->icons[i].base_y - ctx->scroll_y + scr_cy;
              float dx = (float)point.x - sx;
              float dy = (float)point.y - sy;
              float r = (float)AI_WATCH_MENU_ICON_SIZE * 0.4f;

              if (dx * dx + dy * dy < r * r)
                {
                  /* Launch straight away. Previously a tap on a
                   * non-focused icon only panned it to the centre and a
                   * second tap was needed to open it; the panned frame
                   * (icons pushed off the edge, nothing happening) reads
                   * as a frozen screen, and over the 3-10 fps remote link
                   * a two-tap flow is unusable.
                   */

                  ai_watch_open_app(watch, ctx->icons[i].app_index);
                  break;
                }
            }
        }
    }
}

/****************************************************************************
 * Private Functions - Theme Management
 ****************************************************************************/

static lv_color_t ai_watch_theme_bg(int theme)
{
  switch (theme)
    {
      case AI_WATCH_THEME_LIGHT:
        return lv_color_white();
      case AI_WATCH_THEME_BLUE:
        return lv_color_make(10, 25, 50);
      case AI_WATCH_THEME_DARK:
      default:
        return lv_color_black();
    }
}

static lv_color_t ai_watch_theme_text(int theme)
{
  switch (theme)
    {
      case AI_WATCH_THEME_LIGHT:
        return lv_color_black();
      case AI_WATCH_THEME_BLUE:
        return lv_color_make(200, 220, 255);
      case AI_WATCH_THEME_DARK:
      default:
        return lv_color_white();
    }
}

static lv_color_t ai_watch_theme_accent(int theme)
{
  switch (theme)
    {
      case AI_WATCH_THEME_LIGHT:
        return lv_color_make(0, 122, 255);
      case AI_WATCH_THEME_BLUE:
        return lv_color_make(100, 180, 255);
      case AI_WATCH_THEME_DARK:
      default:
        return lv_color_make(0, 191, 255);
    }
}

static lv_color_t ai_watch_theme_secondary(int theme)
{
  switch (theme)
    {
      case AI_WATCH_THEME_LIGHT:
        return lv_color_make(100, 100, 100);
      case AI_WATCH_THEME_BLUE:
        return lv_color_make(120, 150, 200);
      case AI_WATCH_THEME_DARK:
      default:
        return lv_color_make(180, 180, 180);
    }
}

static lv_color_t ai_watch_theme_btn_bg(int theme)
{
  switch (theme)
    {
      case AI_WATCH_THEME_LIGHT:
        return lv_color_make(230, 230, 230);
      case AI_WATCH_THEME_BLUE:
        return lv_color_make(20, 45, 80);
      case AI_WATCH_THEME_DARK:
      default:
        return lv_color_make(40, 40, 40);
    }
}

/* Recolour the two bar widgets of the settings page. They carry their own
 * part-level colours (track, indicator, knob, segment fill) that the generic
 * "walk the children and set the text colour" pass in ai_watch_update_theme()
 * cannot express.
 */

static void ai_watch_settings_bar_paint(FAR struct ai_watch_s *watch, int theme)
{
  int i;

  if (watch->brightness_slider != NULL)
    {
      lv_obj_set_style_bg_color(watch->brightness_slider,
                                ai_watch_theme_secondary(theme), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(watch->brightness_slider, LV_OPA_30,
                              LV_PART_MAIN);
      lv_obj_set_style_bg_color(watch->brightness_slider,
                                ai_watch_theme_accent(theme), LV_PART_INDICATOR);
      lv_obj_set_style_bg_opa(watch->brightness_slider, LV_OPA_COVER,
                              LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(watch->brightness_slider,
                                ai_watch_theme_accent(theme), LV_PART_KNOB);
      lv_obj_set_style_bg_opa(watch->brightness_slider, LV_OPA_COVER,
                              LV_PART_KNOB);
      lv_obj_set_style_border_color(watch->brightness_slider,
                                    ai_watch_theme_bg(theme), LV_PART_KNOB);
      lv_obj_set_style_text_color(watch->brightness_value,
                                  ai_watch_theme_accent(theme), 0);
    }

  for (i = 0; i < AI_WATCH_THEME_COUNT; i++)
    {
      FAR lv_obj_t *seg = watch->theme_seg[i];
      bool selected;

      if (seg == NULL)
        {
          continue;
        }

      selected = (i == theme);

      lv_obj_set_style_bg_color(seg,
                                selected ? ai_watch_theme_accent(theme) :
                                           ai_watch_theme_secondary(theme), 0);
      lv_obj_set_style_bg_opa(seg, selected ? LV_OPA_COVER : LV_OPA_20, 0);
      lv_obj_set_style_text_color(watch->theme_seg_label[i],
                                  selected ? ai_watch_theme_bg(theme) :
                                             ai_watch_theme_text(theme), 0);
    }
}

/* Battery gauge geometry. The shell is 26x13 with a 2 px border, leaving a
 * 22x9 well; the fill grows from 1 px to 20 px as the charge goes 0..100%.
 */

#define AI_WATCH_BATT_FILL_MAX  20
#define AI_WATCH_BATT_LOW_PCT   20
#define AI_WATCH_BATT_LOW_COLOR 0xe53935

static void ai_watch_battery_update(FAR struct ai_watch_s *watch)
{
  int pct;
  int width;
  lv_color_t color;
  char buf[12];

  if (watch->battery_label == NULL || watch->battery_fill == NULL)
    {
      return;
    }

  pct = ai_watch_battery_percent();
  if (pct < 0)
    {
      return;
    }

  if (pct == watch->battery_shown_pct)
    {
      return;
    }

  watch->battery_shown_pct = pct;

  snprintf(buf, sizeof(buf), "%d%%", pct);
  lv_label_set_text(watch->battery_label, buf);

  width = pct * AI_WATCH_BATT_FILL_MAX / 100;
  if (width < 2)
    {
      width = 2;
    }

  lv_obj_set_width(watch->battery_fill, width);

  color = (pct <= AI_WATCH_BATT_LOW_PCT)
              ? lv_color_hex(AI_WATCH_BATT_LOW_COLOR)
              : ai_watch_theme_accent(watch->current_theme);

  lv_obj_set_style_bg_color(watch->battery_fill, color, 0);
  lv_obj_set_style_border_color(watch->battery_shell, color, 0);
  lv_obj_set_style_text_color(watch->battery_label, color, 0);
}

static void ai_watch_update_theme(FAR struct ai_watch_s *watch, int theme)
{
  uint32_t settings_child_count;
  uint32_t i;

  if (theme < 0 || theme >= AI_WATCH_THEME_COUNT ||
      theme == watch->current_theme)
    {
      return;
    }

  watch->current_theme = theme;
  printf("Theme changed to %d\n", theme);

  /* Update Home page */

  lv_obj_set_style_bg_color(watch->home_screen,
                            ai_watch_theme_bg(theme), 0);
  lv_obj_set_style_text_color(watch->home_title_label,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_color(watch->time_label,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_color(watch->seconds_label,
                              ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_color(watch->date_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_color(watch->unread_label,
                              ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_color(watch->bt_label,
                              ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_color(watch->home_hint_label,
                              ai_watch_theme_secondary(theme), 0);

  /* Repaint the gauge in the new palette (force, not value-driven) */

  watch->battery_shown_pct = -2;
  ai_watch_battery_update(watch);

  /* Update the alert banner if it is on screen */

  ai_watch_alert_apply_theme(theme);

  /* Update App List page (just background) */

  if (watch->app_list_screen != NULL)
    {
      lv_obj_set_style_bg_color(watch->app_list_screen,
                                ai_watch_theme_bg(theme), 0);
    }

  /* Update Settings page */

  if (watch->settings_screen != NULL)
    {
      lv_obj_set_style_bg_color(watch->settings_screen,
                                ai_watch_theme_bg(theme), 0);

      settings_child_count = lv_obj_get_child_count(watch->settings_list);

      lv_obj_set_style_bg_color(watch->settings_list,
                                ai_watch_theme_secondary(theme),
                                LV_PART_SCROLLBAR);

      for (i = 0; i < settings_child_count; i++)
        {
          FAR lv_obj_t *child = lv_obj_get_child(watch->settings_list, i);

          if (child == watch->about_label)
            {
              lv_obj_set_style_text_color(
                  child, ai_watch_theme_secondary(theme), 0);
            }
          else if (child == watch->ble_state_label)
            {
              lv_obj_set_style_text_color(
                  child, ai_watch_theme_accent(theme), 0);
            }
          else if (child == watch->bt_switch || child == watch->auto_switch)
            {
              lv_obj_set_style_bg_color(child,
                                        ai_watch_theme_secondary(theme),
                                        LV_PART_MAIN);
              lv_obj_set_style_bg_color(child,
                                        ai_watch_theme_accent(theme),
                                        LV_PART_INDICATOR |
                                        LV_STATE_CHECKED);
            }
          else if (child != watch->theme_seg_bar &&
                   child != watch->brightness_slider &&
                   child != watch->brightness_value)
            {
              lv_obj_set_style_text_color(
                  child, ai_watch_theme_text(theme), 0);
            }
        }

      ai_watch_settings_bar_paint(watch, theme);
    }
}

/****************************************************************************
 * Private Functions - State Management
 ****************************************************************************/

static void ai_watch_update_bt_label(FAR struct ai_watch_s *watch)
{
  FAR const char *text;

  text = ai_watch_ble_get_status_text(ai_watch_ble_get_state());
  lv_label_set_text(watch->bt_label, text);

  if (watch->ble_state_label != NULL)
    {
      lv_label_set_text(watch->ble_state_label, text);
    }
}

/****************************************************************************
 * Private Functions - Page Management
 ****************************************************************************/

static void ai_watch_page_manager_init(FAR struct ai_watch_s *watch)
{
  watch->page_stack_top = 0;
  watch->current_page = AI_WATCH_PAGE_HOME;
  watch->app_page_screen = NULL;
  watch->active_app_index = -1;
  watch->page_stack[0] = AI_WATCH_PAGE_HOME;
}

static void ai_watch_push_page(FAR struct ai_watch_s *watch, int page)
{
  FAR lv_obj_t *target = NULL;
  lv_scr_load_anim_t anim;

  if (watch->page_stack_top >= AI_WATCH_PAGE_STACK_MAX - 1)
    {
      printf("ERROR: Page stack overflow\n");
      return;
    }

  if (page < AI_WATCH_PAGE_FIXED_COUNT)
    {
      target = watch->fixed_pages[page];
    }
  else if (page == AI_WATCH_PAGE_APP_BASE &&
           watch->app_page_screen != NULL)
    {
      target = watch->app_page_screen;
    }

  if (target == NULL)
    {
      return;
    }

  watch->page_stack_top++;
  watch->page_stack[watch->page_stack_top] = page;
  watch->current_page = page;

  anim = (page == AI_WATCH_PAGE_APP_LIST) ?
         LV_SCR_LOAD_ANIM_FADE_ON :
         LV_SCR_LOAD_ANIM_MOVE_LEFT;

  lv_scr_load_anim(target, anim, 200, 0, false);
  printf("Push page %d (stack depth %d)\n", page,
         watch->page_stack_top);
}

static void ai_watch_pop_page(FAR struct ai_watch_s *watch)
{
  int prev_page;
  FAR lv_obj_t *target;
  lv_scr_load_anim_t anim;

  if (watch->page_stack_top <= 0)
    {
      return;
    }

  watch->page_stack_top--;
  prev_page = watch->page_stack[watch->page_stack_top];

  /* If leaving a dynamic app page, mark for cleanup but do NOT delete
   * the screen object yet -- lv_scr_load_anim will switch away from it
   * and the old screen is auto-deleted by LVGL after animation.
   * We just clear our reference so a new app page can be created later.
   */

  if (watch->current_page == AI_WATCH_PAGE_APP_BASE)
    {
      if (watch->active_app_index >= 0 &&
          watch->active_app_index < (int)AI_WATCH_APP_COUNT &&
          g_app_registry[watch->active_app_index].destroy_cb != NULL)
        {
          g_app_registry[watch->active_app_index].destroy_cb(watch);
        }

      watch->app_page_screen = NULL;
      watch->active_app_index = -1;
    }

  if (prev_page < AI_WATCH_PAGE_FIXED_COUNT)
    {
      target = watch->fixed_pages[prev_page];
    }
  else
    {
      return;
    }

  if (target == NULL)
    {
      return;
    }

  watch->current_page = prev_page;

  anim = (prev_page == AI_WATCH_PAGE_HOME) ?
         LV_SCR_LOAD_ANIM_FADE_ON :
         LV_SCR_LOAD_ANIM_MOVE_RIGHT;

  lv_scr_load_anim(target, anim, 200, 0, false);
  printf("Pop to page %d (stack depth %d)\n", prev_page,
         watch->page_stack_top);
}

/* Swipe-right shortcut: straight back to home from any app or settings
 * page. The home screen itself and the hex app-list page are excluded
 * (their gestures must not be affected). Mirrors the dynamic-app
 * cleanup done by ai_watch_pop_page; the old screen object is freed by
 * LVGL once the load animation finishes.
 */

static void ai_watch_go_home(FAR struct ai_watch_s *watch)
{
  if (watch->current_page == AI_WATCH_PAGE_HOME ||
      watch->current_page == AI_WATCH_PAGE_APP_LIST)
    {
      return;
    }

  if (watch->current_page == AI_WATCH_PAGE_APP_BASE)
    {
      if (watch->active_app_index >= 0 &&
          watch->active_app_index < (int)AI_WATCH_APP_COUNT &&
          g_app_registry[watch->active_app_index].destroy_cb != NULL)
        {
          g_app_registry[watch->active_app_index].destroy_cb(watch);
        }

      watch->app_page_screen = NULL;
      watch->active_app_index = -1;
    }

  watch->page_stack_top = 0;
  watch->page_stack[0] = AI_WATCH_PAGE_HOME;
  watch->current_page = AI_WATCH_PAGE_HOME;

  lv_scr_load_anim(watch->home_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                   200, 0, false);
  printf("Swipe: back to home\n");
}

static void ai_watch_open_app(FAR struct ai_watch_s *watch, int app_index)
{
  const struct ai_app_desc_s *app;

  if (app_index < 0 || app_index >= (int)AI_WATCH_APP_COUNT)
    {
      return;
    }

  app = &g_app_registry[app_index];

  /* Special case: Settings uses the pre-created settings page */

  if (app_index == 3)
    {
      ai_watch_push_page(watch, AI_WATCH_PAGE_SETTINGS);
      return;
    }

  if (!app->available || app->create_cb == NULL)
    {
      FAR lv_obj_t *msgbox = lv_msgbox_create(NULL);

      lv_msgbox_add_title(msgbox, "Coming Soon");
      lv_msgbox_add_text(msgbox, "This app is under development.");
      lv_msgbox_add_close_button(msgbox);
      return;
    }

  app->create_cb(watch);

  if (watch->app_page_screen != NULL)
    {
      watch->active_app_index = app_index;
      watch->page_stack_top++;
      watch->page_stack[watch->page_stack_top] =
          AI_WATCH_PAGE_APP_BASE;
      watch->current_page = AI_WATCH_PAGE_APP_BASE;

      lv_scr_load_anim(watch->app_page_screen,
                       LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
      printf("Opened app: %s\n", app->name);
    }
}

/****************************************************************************
 * Private Functions - Timer App
 ****************************************************************************/

static void ai_watch_timer_update_cb(FAR lv_timer_t *timer)
{
  FAR struct ai_watch_s *watch = lv_timer_get_user_data(timer);
  uint32_t total_ms;
  uint32_t minutes;
  uint32_t seconds;
  uint32_t centiseconds;
  char buf[16];

  if (watch->timer.running)
    {
      total_ms = watch->timer.elapsed_ms +
                 (lv_tick_get() - watch->timer.start_tick);
    }
  else
    {
      total_ms = watch->timer.elapsed_ms;
    }

  minutes = total_ms / 60000;
  seconds = (total_ms % 60000) / 1000;
  centiseconds = (total_ms % 1000) / 10;

  snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu",
           (unsigned long)minutes,
           (unsigned long)seconds,
           (unsigned long)centiseconds);
  lv_label_set_text(watch->timer.time_label, buf);
}

static void ai_watch_timer_start_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);
  FAR lv_obj_t *btn = lv_event_get_target(e);
  FAR lv_obj_t *label = lv_obj_get_child(btn, 0);

  if (!watch->timer.running)
    {
      watch->timer.start_tick = lv_tick_get();
      watch->timer.running = true;
      lv_label_set_text(label, LV_SYMBOL_PAUSE " Pause");
    }
  else
    {
      watch->timer.elapsed_ms +=
          lv_tick_get() - watch->timer.start_tick;
      watch->timer.running = false;
      lv_label_set_text(label, LV_SYMBOL_PLAY " Start");
    }
}

static void ai_watch_timer_reset_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);

  watch->timer.running = false;
  watch->timer.elapsed_ms = 0;
  watch->timer.start_tick = 0;
  lv_label_set_text(watch->timer.time_label, "00:00.00");

  /* Reset start button text */

  FAR lv_obj_t *start_btn =
      lv_obj_get_child(watch->app_page_screen, 1);

  if (start_btn != NULL)
    {
      FAR lv_obj_t *label = lv_obj_get_child(start_btn, 0);

      if (label != NULL)
        {
          lv_label_set_text(label, LV_SYMBOL_PLAY " Start");
        }
    }
}

static void ai_watch_timer_back_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);

  ai_watch_pop_page(watch);
}

static void ai_watch_create_timer_app(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen;
  FAR lv_obj_t *title;
  FAR lv_obj_t *time_lbl;
  FAR lv_obj_t *start_btn;
  FAR lv_obj_t *start_lbl;
  FAR lv_obj_t *reset_btn;
  FAR lv_obj_t *reset_lbl;
  FAR lv_obj_t *back_btn;
  FAR lv_obj_t *back_lbl;
  int theme = watch->current_theme;

  screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(theme), 0);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

  title = lv_label_create(screen);
  lv_label_set_text(title, "Timer");
  lv_obj_set_style_text_color(title, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

  time_lbl = lv_label_create(screen);
  lv_label_set_text(time_lbl, "00:00.00");
  lv_obj_set_style_text_color(time_lbl, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_48, 0);
  lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, -50);
  watch->timer.time_label = time_lbl;

  start_btn = lv_btn_create(screen);
  lv_obj_set_size(start_btn, 120, 45);
  lv_obj_set_style_bg_color(start_btn, ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_radius(start_btn, 12, 0);
  lv_obj_align(start_btn, LV_ALIGN_CENTER, -70, 40);
  start_lbl = lv_label_create(start_btn);
  lv_label_set_text(start_lbl, LV_SYMBOL_PLAY " Start");
  lv_obj_set_style_text_color(start_lbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(start_lbl, &lv_font_montserrat_16, 0);
  lv_obj_center(start_lbl);
  lv_obj_add_event_cb(start_btn, ai_watch_timer_start_cb,
                      LV_EVENT_CLICKED, watch);

  reset_btn = lv_btn_create(screen);
  lv_obj_set_size(reset_btn, 120, 45);
  lv_obj_set_style_bg_color(reset_btn,
                            ai_watch_theme_btn_bg(theme), 0);
  lv_obj_set_style_radius(reset_btn, 12, 0);
  lv_obj_align(reset_btn, LV_ALIGN_CENTER, 70, 40);
  reset_lbl = lv_label_create(reset_btn);
  lv_label_set_text(reset_lbl, LV_SYMBOL_REFRESH " Reset");
  lv_obj_set_style_text_color(reset_lbl, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(reset_lbl, &lv_font_montserrat_16, 0);
  lv_obj_center(reset_lbl);
  lv_obj_add_event_cb(reset_btn, ai_watch_timer_reset_cb,
                      LV_EVENT_CLICKED, watch);

  back_btn = lv_btn_create(screen);
  lv_obj_set_size(back_btn, 120, 45);
  lv_obj_set_style_bg_color(back_btn, ai_watch_theme_btn_bg(theme), 0);
  lv_obj_set_style_radius(back_btn, 12, 0);
  lv_obj_align(back_btn, LV_ALIGN_CENTER, 0, 110);
  back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_color(back_lbl, ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_16, 0);
  lv_obj_center(back_lbl);
  lv_obj_add_event_cb(back_btn, ai_watch_timer_back_cb,
                      LV_EVENT_CLICKED, watch);

  watch->timer.running = false;
  watch->timer.elapsed_ms = 0;
  watch->timer.start_tick = 0;

  watch->timer.update_timer = lv_timer_create(
      ai_watch_timer_update_cb, AI_WATCH_TIMER_UPDATE_MS, watch);

  watch->app_page_screen = screen;
}

static void ai_watch_destroy_timer_app(FAR struct ai_watch_s *watch)
{
  /* Stop the update timer */

  if (watch->timer.update_timer != NULL)
    {
      lv_timer_del(watch->timer.update_timer);
      watch->timer.update_timer = NULL;
    }

  /* Reset timer state */

  watch->timer.running = false;
  watch->timer.elapsed_ms = 0;
  watch->timer.start_tick = 0;
  watch->timer.time_label = NULL;

  /* NOTE: Do NOT delete app_page_screen here.
   * The screen is being animated by lv_scr_load_anim.
   * LVGL will handle the old screen lifecycle.
   * We just clear our reference so a new page can be created.
   */

  watch->app_page_screen = NULL;
}

/****************************************************************************
 * Private Functions - Reminder App
 ****************************************************************************/

#define REMINDER_APP_MAX_LABELS  8

/* True when s is pure ASCII (no UTF-8 multibyte sequences) */

static bool ai_watch_text_is_ascii(FAR const char *s)
{
  if (s == NULL)
    {
      return true;
    }

  while (*s != '\0')
    {
      if ((*s++ & 0x80) != 0)
        {
          return false;
        }
    }

  return true;
}

struct ai_reminder_app_s
{
  FAR lv_obj_t *list_obj;
  FAR lv_obj_t *item_labels[REMINDER_APP_MAX_LABELS];
  FAR lv_obj_t *empty_label;
  FAR lv_timer_t *refresh_timer;
};

static struct ai_reminder_app_s g_reminder_app;

static void ai_watch_reminder_refresh_cb(FAR lv_timer_t *timer)
{
  FAR struct ai_watch_s *watch = lv_timer_get_user_data(timer);
  FAR struct ai_watch_reminder_store_s *store;
  int theme = watch->current_theme;
  int i;

  store = ai_watch_ble_get_reminders();
  if (store == NULL || !store->pending)
    {
      return;
    }

  store->pending = false;

  /* Hide empty label if we have reminders */

  if (g_reminder_app.empty_label != NULL)
    {
      if (store->count > 0)
        {
          lv_obj_add_flag(g_reminder_app.empty_label,
                          LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_clear_flag(g_reminder_app.empty_label,
                            LV_OBJ_FLAG_HIDDEN);
        }
    }

  /* Update item labels */

  for (i = 0; i < REMINDER_APP_MAX_LABELS; i++)
    {
      FAR lv_obj_t *lbl = g_reminder_app.item_labels[i];

      if (lbl == NULL)
        {
          continue;
        }

      if (store->items[i].id != 0)
        {
          FAR const struct ai_watch_reminder_s *item =
              &store->items[i];
          char buf[40];
          char when[8];
          bool is_read = (item->flags & AI_WATCH_REMINDER_FLAG_READ) != 0;

          /* Trigger time in watch-local time (empty if unset) */

          when[0] = '\0';
          if (item->timestamp >= AI_WATCH_TS_MIN_UTC)
            {
              struct tm tm;

              if (ai_watch_ble_localtime((time_t)item->timestamp, &tm)
                  != NULL)
                {
                  snprintf(when, sizeof(when), "%02d:%02d ",
                           tm.tm_hour, tm.tm_min);
                }
            }

          /* No icon glyphs here: the CJK font deliberately carries no
           * LV_SYMBOL_* glyphs (see fonts/gen_cjk_font.sh), so symbols
           * in a CJK label would render as placeholder boxes.
           */

          snprintf(buf, sizeof(buf), "%s%s", when, item->title);
          lv_label_set_text(lbl, buf);

          /* Chinese titles need the CJK font; keep Montserrat for
           * pure-ASCII ones so glyphs stay consistent.
           */

          lv_obj_set_style_text_font(
              lbl, ai_watch_text_is_ascii(item->title) ?
              &lv_font_montserrat_16 : &ai_watch_font_cjk_16, 0);

          if (is_read)
            {
              lv_obj_set_style_text_color(
                  lbl, ai_watch_theme_secondary(theme), 0);
            }
          else
            {
              lv_obj_set_style_text_color(
                  lbl, ai_watch_theme_text(theme), 0);
            }

          lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ai_watch_reminder_item_click_cb(lv_event_t *e)
{
  FAR struct ai_watch_reminder_store_s *store;
  uint8_t slot = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  bool is_read;

  store = ai_watch_ble_get_reminders();
  if (store == NULL || slot >= AI_WATCH_REMINDER_MAX ||
      store->items[slot].id == 0)
    {
      return;
    }

  /* Tap toggles read/unread so a mis-tap can be undone */

  is_read = (store->items[slot].flags & AI_WATCH_REMINDER_FLAG_READ) != 0;
  ai_watch_reminder_set_read(slot, !is_read);
}

static void ai_watch_reminder_item_long_cb(lv_event_t *e)
{
  uint8_t slot = (uint8_t)(intptr_t)lv_event_get_user_data(e);

  ai_watch_reminder_delete(slot);
}

static void ai_watch_reminder_clear_cb(lv_event_t *e)
{
  (void)e;

  ai_watch_reminders_clear();
}

static void ai_watch_reminder_back_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);

  ai_watch_pop_page(watch);
}

static void ai_watch_create_reminder_app(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen;
  FAR lv_obj_t *title;
  FAR lv_obj_t *back_btn;
  FAR lv_obj_t *back_lbl;
  FAR lv_obj_t *clear_btn;
  FAR lv_obj_t *clear_lbl;
  FAR lv_obj_t *list;
  FAR lv_obj_t *empty_lbl;
  int theme = watch->current_theme;
  int i;

  screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(theme), 0);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

  title = lv_label_create(screen);
  lv_label_set_text(title, "Reminders");
  lv_obj_set_style_text_color(title, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

  /* Scrollable list area */

  list = lv_obj_create(screen);
  lv_obj_set_size(list, 340, 320);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 65);
  lv_obj_set_style_bg_color(list, ai_watch_theme_bg(theme), 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 8, 0);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  /* Empty state label */

  empty_lbl = lv_label_create(list);
  lv_label_set_text(empty_lbl, "No reminders\n\nWaiting for data\n"
                    "from phone...");
  lv_obj_set_style_text_color(empty_lbl,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(empty_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(empty_lbl, 60, 0);
  g_reminder_app.empty_label = empty_lbl;

  /* Pre-create item labels (hidden until data arrives). Tap toggles
   * read/unread, long-press deletes the entry.
   */

  for (i = 0; i < REMINDER_APP_MAX_LABELS; i++)
    {
      FAR lv_obj_t *lbl = lv_label_create(list);

      lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
      lv_obj_set_width(lbl, 320);
      lv_obj_set_style_text_color(lbl, ai_watch_theme_text(theme), 0);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
      lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_ext_click_area(lbl, 8);
      lv_obj_add_event_cb(lbl, ai_watch_reminder_item_click_cb,
                          LV_EVENT_CLICKED, (void *)(intptr_t)i);
      lv_obj_add_event_cb(lbl, ai_watch_reminder_item_long_cb,
                          LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
      g_reminder_app.item_labels[i] = lbl;
    }

  g_reminder_app.list_obj = list;

  /* Back button (left) and Clear-all button (right) */

  back_btn = lv_btn_create(screen);
  lv_obj_set_size(back_btn, 120, 45);
  lv_obj_set_style_bg_color(back_btn, ai_watch_theme_btn_bg(theme), 0);
  lv_obj_set_style_radius(back_btn, 12, 0);
  lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, -75, -20);
  back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_color(back_lbl,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_16, 0);
  lv_obj_center(back_lbl);
  lv_obj_add_event_cb(back_btn, ai_watch_reminder_back_cb,
                      LV_EVENT_CLICKED, watch);

  clear_btn = lv_btn_create(screen);
  lv_obj_set_size(clear_btn, 120, 45);
  lv_obj_set_style_bg_color(clear_btn, ai_watch_theme_btn_bg(theme), 0);
  lv_obj_set_style_radius(clear_btn, 12, 0);
  lv_obj_align(clear_btn, LV_ALIGN_BOTTOM_MID, 75, -20);
  clear_lbl = lv_label_create(clear_btn);
  lv_label_set_text(clear_lbl, "Clear all");
  lv_obj_set_style_text_color(clear_lbl,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(clear_lbl, &lv_font_montserrat_16, 0);
  lv_obj_center(clear_lbl);
  lv_obj_add_event_cb(clear_btn, ai_watch_reminder_clear_cb,
                      LV_EVENT_CLICKED, watch);

  /* Refresh timer — checks for new reminder data every 500ms */

  g_reminder_app.refresh_timer = lv_timer_create(
      ai_watch_reminder_refresh_cb, 500, watch);

  /* Trigger initial refresh */

  {
    FAR struct ai_watch_reminder_store_s *store =
        ai_watch_ble_get_reminders();

    if (store != NULL)
      {
        store->pending = true;
      }
  }

  watch->app_page_screen = screen;
}

static void ai_watch_destroy_reminder_app(FAR struct ai_watch_s *watch)
{
  if (g_reminder_app.refresh_timer != NULL)
    {
      lv_timer_del(g_reminder_app.refresh_timer);
      g_reminder_app.refresh_timer = NULL;
    }

  memset(&g_reminder_app, 0, sizeof(g_reminder_app));
  watch->app_page_screen = NULL;
}

/****************************************************************************
 * Private Functions - Incoming Alert Banner
 *
 * When the phone pushes a reminder or notification, a card is shown on
 * LVGL's top layer (above every page). It auto-hides after a few seconds
 * or on tap. Data arrives via ai_watch_ble_take_alert(), polled from the
 * main loop, so the banner is only ever touched from the LVGL thread.
 ****************************************************************************/

#define AI_WATCH_ALERT_SHOW_MS      6000
#define AI_WATCH_ALERT_WIDTH        358
#define AI_WATCH_BRIEF_WIDTH        380
/* Scrollable extent of the brief page: content ends at the hint label
 * (y=486 + ~19 px) on the 450 px panel. The page is laid out with
 * absolute coordinates, so this is a build-time constant -- keep it in
 * sync when the layout changes. lv_obj_get_scroll_bottom() was observed
 * to return unreliable values here (scroll overshot into black). */
#define AI_WATCH_BRIEF_SCROLL_MAX   60

/* Headless mode: render into the LVGL buffer and never touch the panel.
 *
 * With the panel physically disconnected, every flush still goes through
 * the LCD driver's putarea(), which waits for hardware that is not there
 * and times out -- the UI crawls to one frame every several seconds. The
 * screen dump only needs the rendered buffer, so skip the panel entirely.
 *
 * Set to 0 once a working panel is connected again.
 */

#define AI_WATCH_HEADLESS           1

/* Length of the screen-change animation. Without a panel the transition is
 * only ever seen through the 5 fps remote preview, where a fade that takes
 * longer than a couple of frames is indistinguishable from a freeze -- and
 * worse, clicks during it land on offset screens. Switch instantly instead.
 */

#if AI_WATCH_HEADLESS
#  define AI_WATCH_PAGE_ANIM_MS     0
#else
#  define AI_WATCH_PAGE_ANIM_MS     200
#endif

struct ai_alert_ui_s
{
  FAR lv_obj_t *panel;
  FAR lv_obj_t *type_label;
  FAR lv_obj_t *title_label;
  FAR lv_timer_t *hide_timer;
  bool visible;
};

static struct ai_alert_ui_s g_alert_ui;

static void ai_watch_alert_apply_theme(int theme)
{
  if (g_alert_ui.panel == NULL)
    {
      return;
    }

  lv_obj_set_style_bg_color(g_alert_ui.panel,
                            ai_watch_theme_btn_bg(theme), 0);
  lv_obj_set_style_border_color(g_alert_ui.panel,
                                ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_color(g_alert_ui.type_label,
                              ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_color(g_alert_ui.title_label,
                              ai_watch_theme_text(theme), 0);
}

static void ai_watch_alert_hide(void)
{
  if (g_alert_ui.panel != NULL)
    {
      lv_obj_add_flag(g_alert_ui.panel, LV_OBJ_FLAG_HIDDEN);
    }

  if (g_alert_ui.hide_timer != NULL)
    {
      lv_timer_del(g_alert_ui.hide_timer);
      g_alert_ui.hide_timer = NULL;
    }

  g_alert_ui.visible = false;
}

static void ai_watch_alert_timeout_cb(FAR lv_timer_t *timer)
{
  (void)timer;
  ai_watch_alert_hide();
}

static void ai_watch_alert_click_cb(lv_event_t *e)
{
  (void)e;
  ai_watch_alert_hide();
}

static void ai_watch_alert_show(FAR struct ai_watch_s *watch,
    FAR const struct ai_watch_ble_alert_s *a)
{
  int theme = watch->current_theme;
  FAR const char *type_text;
  FAR const lv_font_t *title_font;

  if (g_alert_ui.panel == NULL)
    {
      FAR lv_obj_t *panel = lv_obj_create(lv_layer_top());

      lv_obj_set_size(panel, AI_WATCH_ALERT_WIDTH, LV_SIZE_CONTENT);
      lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 12);
      lv_obj_set_style_radius(panel, 16, 0);
      lv_obj_set_style_border_width(panel, 2, 0);
      lv_obj_set_style_pad_all(panel, 12, 0);
      lv_obj_set_style_pad_row(panel, 6, 0);
      lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START,
                            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
      lv_obj_add_event_cb(panel, ai_watch_alert_click_cb,
                          LV_EVENT_CLICKED, NULL);

      g_alert_ui.type_label = lv_label_create(panel);
      lv_label_set_long_mode(g_alert_ui.type_label, LV_LABEL_LONG_DOT);
      lv_obj_set_width(g_alert_ui.type_label,
                       AI_WATCH_ALERT_WIDTH - 2 * 12);
      lv_obj_set_style_text_font(g_alert_ui.type_label,
                                 &lv_font_montserrat_16, 0);

      g_alert_ui.title_label = lv_label_create(panel);
      lv_label_set_long_mode(g_alert_ui.title_label, LV_LABEL_LONG_DOT);
      lv_obj_set_width(g_alert_ui.title_label,
                       AI_WATCH_ALERT_WIDTH - 2 * 12);

      g_alert_ui.panel = panel;
    }

  if (a->type == AI_WATCH_BLE_CMD_NOTIFICATION)
    {
      type_text = LV_SYMBOL_ENVELOPE "  Notification";
    }
  else if (a->type == AI_WATCH_BLE_CMD_AI_TEXT)
    {
      type_text = "AI " LV_SYMBOL_RIGHT "  Result";
    }
  else if (a->type == AI_WATCH_BLE_CMD_AI_TIMER)
    {
      type_text = LV_SYMBOL_BELL "  Timer done";
    }
  else if (a->type == AI_WATCH_ALERT_TYPE_FALL)
    {
      type_text = LV_SYMBOL_WARNING "  Suspected fall";
    }
  else if (a->type == AI_WATCH_ALERT_TYPE_FALL_INFO)
    {
      type_text = LV_SYMBOL_OK "  Fall info";
    }
  else if (a->type == AI_WATCH_ALERT_TYPE_SKILL_SIT)
    {
      type_text = LV_SYMBOL_BELL "  \xe4\xb9\x85\xe5\x9d\x90\xe6\x8f\x90\xe9\x86\x92";
    }
  else if (a->type == AI_WATCH_ALERT_TYPE_SKILL_GOAL)
    {
      type_text = LV_SYMBOL_OK "  \xe7\x9b\xae\xe6\xa0\x87\xe8\xbe\xbe\xe6\x88\x90";
    }
  else if (a->type == AI_WATCH_ALERT_TYPE_SKILL_BRIEF)
    {
      type_text = LV_SYMBOL_EDIT "  \xe6\xaf\x8f\xe6\x97\xa5\xe7\xae\x80\xe6\x8a\xa5";
    }
  else
    {
      type_text = LV_SYMBOL_BELL "  Reminder";
    }

  title_font = ai_watch_text_is_ascii(a->title) ?
               &lv_font_montserrat_20 : &ai_watch_font_cjk_16;

  lv_label_set_text(g_alert_ui.type_label, type_text);
  lv_obj_set_style_text_font(g_alert_ui.title_label, title_font, 0);
  lv_label_set_text(g_alert_ui.title_label, a->title);

  ai_watch_alert_apply_theme(theme);
  lv_obj_clear_flag(g_alert_ui.panel, LV_OBJ_FLAG_HIDDEN);
  g_alert_ui.visible = true;

  /* Restart the auto-hide timer for the newest arrival */

  if (g_alert_ui.hide_timer != NULL)
    {
      lv_timer_del(g_alert_ui.hide_timer);
    }

  g_alert_ui.hide_timer = lv_timer_create(ai_watch_alert_timeout_cb,
                                          AI_WATCH_ALERT_SHOW_MS, NULL);
  lv_timer_set_repeat_count(g_alert_ui.hide_timer, 1);
}

/****************************************************************************
 * Name: ai_watch_skill_poll_ui
 *
 * Description:
 *   Drain the device-skill event queue and surface each event as an alert
 *   banner. Skill text is generated here, on the main thread, so the skill
 *   worker never has to format CJK strings or know about LVGL.
 *
 *   The daily brief posts a structured event upward so the phone can add
 *   one sentence of commentary; the banner shows the local summary right
 *   away and is replaced by the phone text through DAILY_BRIEF when it
 *   arrives. With no phone the local summary stands on its own.
 *
 ****************************************************************************/

static void ai_watch_skill_poll_ui(FAR struct ai_watch_s *watch)
{
  struct ai_watch_skill_event_s ev;

  while (ai_watch_skill_poll(&ev))
    {
      struct ai_watch_ble_alert_s alert;

      memset(&alert, 0, sizeof(alert));
      alert.timestamp = (uint32_t)time(NULL);

      switch (ev.type)
        {
          /* Titles are capped by AI_WATCH_REMINDER_TITLE_MAX (24 bytes),
           * which is 8 CJK glyphs. Every string below stays within that
           * budget including its terminator, so snprintf never truncates
           * mid-glyph and produces a broken frame.
           */

          case AI_WATCH_SKILL_EVENT_SIT:
            {
              unsigned mins = (unsigned)(ev.sit_ms / 60000u);

              alert.type = AI_WATCH_ALERT_TYPE_SKILL_SIT;
              snprintf(alert.title, sizeof(alert.title),
                       "\xe5\xb7\xb2\xe4\xb9\x85\xe5\x9d\x90%u"
                       "\xe5\x88\x86\xe9\x92\x9f\xe4\xba\x86",
                       mins > 0 ? mins : 1u);
            }
            break;

          case AI_WATCH_SKILL_EVENT_GOAL:
            {
              unsigned steps = (unsigned)ev.steps;

              alert.type = AI_WATCH_ALERT_TYPE_SKILL_GOAL;
              if (steps <= 9999u)
                {
                  snprintf(alert.title, sizeof(alert.title),
                           "\xe7\x9b\xae\xe6\xa0\x87\xe8\xbe\xbe\xe6\x88\x90"
                           "\xef\xbc\x8c%u\xe6\xad\xa5",
                           steps);
                }
              else
                {
                  snprintf(alert.title, sizeof(alert.title),
                           "\xe7\x9b\xae\xe6\xa0\x87\xe8\xbe\xbe\xe6\x88\x90");
                }
            }
            break;

          case AI_WATCH_SKILL_EVENT_BRIEF:
            {
              unsigned steps = (unsigned)ev.steps;

              /* "看简报 99999 步": 8 glyphs at most, but the digits are
               * the variable part. Over ~9999 steps the count alone would
               * push past 24 bytes, so the wording drops to the action
               * alone -- a truncated number reads worse than none.
               */

              alert.type = AI_WATCH_ALERT_TYPE_SKILL_BRIEF;
              if (steps <= 9999u)
                {
                  snprintf(alert.title, sizeof(alert.title),
                           "\xe7\x9c\x8b\xe7\xae\x80\xe6\x8a\xa5%u\xe6\xad\xa5",
                           steps);
                }
              else
                {
                  snprintf(alert.title, sizeof(alert.title),
                           "\xe7\xae\x80\xe6\x8a\xa5\xe5\xb7\xb2\xe7\x94\x9f\xe6\x88\x90"
                           "\xe8\xaf\xb7\xe5\xb7\xa6\xe6\xbb\x91");
                }
            }
            break;

          default:
            continue;
        }

      ai_watch_alert_show(watch, &alert);
    }
}

#if AI_WATCH_HEADLESS
/* LVGL's time base. lv_nuttx_init normally installs this; skipping that
 * init would leave lv_tick_get() returning a counter that never advances,
 * which silently stops every timer -- input polling and rendering
 * included. This is the whole reason headless mode needs its own source.
 */

static uint32_t ai_watch_headless_tick(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint32_t)ts.tv_sec * 1000u +
                    (uint32_t)(ts.tv_nsec / 1000000));
}

/* Nothing is displayed: mark the frame done at once so LVGL keeps
 * rendering at full speed instead of waiting on absent hardware.
 */

static void ai_watch_headless_flush(FAR lv_display_t *disp,
                                    FAR const lv_area_t *area,
                                    FAR uint8_t *px_map)
{
  LV_UNUSED(area);
  LV_UNUSED(px_map);
  lv_display_flush_ready(disp);
}
#endif

/****************************************************************************
 * Private Functions - Touch probe overlay
 *
 * With the panel dead, touch health has to be judged from the screen dump:
 * press anywhere on the glass and the next frame carries a marker plus the
 * coordinates the driver reported. The marker lingers briefly so a single
 * tap is still visible in a frame captured a moment later.
 ****************************************************************************/

static void ai_watch_touch_probe_poll(FAR struct ai_watch_s *watch)
{
  /* DISABLED 2026-11-01: the probe was a bring-up diagnostic for the dead
   * panel. With RENDER_MODE_FULL every marker show/move invalidates and
   * redraws the whole 351 KB framebuffer (~1.2 s measured), which stalls
   * the main loop for the entire gesture - the first touch after a quiet
   * period swallowed swipes whole and they degraded into taps ("右滑被
   * 识别为点击"). The virtual input path is covered by
   * tools/shot_selftest.py instead; re-enable only for touch debugging,
   * one render cost at a time. */

  return;

  static FAR lv_obj_t *dot;
  static FAR lv_obj_t *tag;
  static uint32_t hold_until;
  FAR lv_obj_t *top;
  lv_point_t p;
  char buf[24];
  uint32_t now;

  if (watch->touch_indev == NULL)
    {
      return;
    }

  if (dot == NULL)
    {
      top = lv_layer_top();

      dot = lv_obj_create(top);
      lv_obj_set_size(dot, 28, 28);
      lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(dot, lv_color_hex(0xff3b30), 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_60, 0);
      lv_obj_set_style_border_width(dot, 2, 0);
      lv_obj_set_style_border_color(dot, lv_color_hex(0xffffff), 0);
      lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);

      tag = lv_label_create(top);
      lv_obj_set_style_text_color(tag, lv_color_hex(0xffffff), 0);
      lv_obj_set_style_text_font(tag, &lv_font_montserrat_16, 0);
      lv_label_set_text(tag, "");
    }

  now = lv_tick_get();

  if (lv_indev_get_state(watch->touch_indev) == LV_INDEV_STATE_PRESSED)
    {
      lv_indev_get_point(watch->touch_indev, &p);

      lv_obj_set_pos(dot, p.x - 14, p.y - 14);
      lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);

      snprintf(buf, sizeof(buf), "%d,%d", (int)p.x, (int)p.y);
      lv_label_set_text(tag, buf);
      lv_obj_set_pos(tag, p.x + 18, p.y - 8);

      hold_until = now + 2500;
    }
  else if (hold_until != 0 && (int32_t)(now - hold_until) >= 0)
    {
      lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(tag, "");
      hold_until = 0;
    }
}

/****************************************************************************
 * Private Functions - Exercise App (motion service UI)
 *
 * Pure consumer of the background motion service (ai_watch_motion.c):
 * every value on this page comes from the service snapshot; the page
 * never touches the IMU itself. The recording toggle and the fall-test
 * button drive the service for the M4 test-set capture and regression.
 ****************************************************************************/

/* Pure consumer of the background motion service (ai_watch_motion.c):
 * every value on this page comes from the service snapshot; the page
 * never touches the IMU itself. The recording toggle and the fall-test
 * button drive the service for the M4 test-set capture and regression.
 */

struct ai_exercise_app_s
{
  FAR lv_obj_t *steps_label;
  FAR lv_obj_t *info_label;       /* distance + activity class */
  FAR lv_obj_t *status_label;     /* IMU state + fall detector state */
  FAR lv_obj_t *mag_label;        /* live |a| readout */
  FAR lv_obj_t *mag_bar;          /* live |a| bar (0..4000 mg) */
  FAR lv_obj_t *rec_btn;
  FAR lv_obj_t *rec_lbl;
  FAR lv_timer_t *update_timer;
  bool last_recording;
};

static struct ai_exercise_app_s g_ex_app;

/* Full-screen modal countdown for the "suspected fall" alarm */

struct ai_fall_ui_s
{
  FAR lv_obj_t *panel;
  FAR lv_obj_t *cd_label;
  bool visible;
};

static struct ai_fall_ui_s g_fall_ui;

static void ai_watch_exercise_update_cb(FAR lv_timer_t *timer)
{
  struct ai_watch_motion_snapshot_s snap;
  FAR const char *act_text;
  FAR const char *fall_text;
  char buf[96];

  (void)timer;

  ai_watch_motion_get_snapshot(&snap);

  snprintf(buf, sizeof(buf), "%lu", (unsigned long)snap.steps_today);
  lv_label_set_text(g_ex_app.steps_label, buf);

  act_text = (snap.activity == AI_WATCH_MOTION_RUN) ? "Running" :
             (snap.activity == AI_WATCH_MOTION_WALK) ? "Walking" :
             "Rest";
  snprintf(buf, sizeof(buf), "%lu m   " LV_SYMBOL_BULLET "   %s",
           (unsigned long)snap.distance_m, act_text);
  lv_label_set_text(g_ex_app.info_label, buf);

  if (!snap.sensor_ok)
    {
      fall_text = "IMU: offline";
    }
  else
    {
      fall_text =
        (snap.fall_state == FALL_STATE_ALARM) ? "Fall: countdown!" :
        (snap.fall_state == FALL_STATE_IMPACT) ? "Fall: impact..." :
        "Fall detect: Armed";
    }

  snprintf(buf, sizeof(buf), "%s   " LV_SYMBOL_BULLET
           "   Window std: %u mg",
           fall_text, (unsigned)snap.activity_std_mg);
  lv_label_set_text(g_ex_app.status_label, buf);
  lv_obj_set_style_text_color(g_ex_app.status_label,
                              snap.sensor_ok ?
                              lv_color_make(0, 200, 0) :
                              lv_color_make(255, 180, 0), 0);

  snprintf(buf, sizeof(buf), "|a|  %lu mg",
           (unsigned long)snap.last_mag_mg);
  lv_label_set_text(g_ex_app.mag_label, buf);
  lv_bar_set_value(g_ex_app.mag_bar,
                   (int32_t)((snap.last_mag_mg > 4000) ? 4000 :
                             snap.last_mag_mg), LV_ANIM_OFF);

  if (snap.recording)
    {
      snprintf(buf, sizeof(buf), "Stop (%lu)",
               (unsigned long)snap.record_lines);
    }
  else
    {
      strncpy(buf, "Record", sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
    }

  lv_label_set_text(g_ex_app.rec_lbl, buf);
  g_ex_app.last_recording = snap.recording;
}

static void ai_watch_exercise_rec_cb(lv_event_t *e)
{
  (void)e;

  if (g_ex_app.last_recording)
    {
      ai_watch_motion_record_stop();
    }
  else
    {
      if (!ai_watch_motion_record_start())
        {
          printf("Exercise: record start failed (sensor down?)\n");
        }
    }
}

static void ai_watch_exercise_test_cb(lv_event_t *e)
{
  (void)e;

  if (!ai_watch_motion_test_trigger())
    {
      printf("Exercise: test trigger busy (fall already pending)\n");
    }
}

static void ai_watch_exercise_back_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);

  ai_watch_pop_page(watch);
}

static void ai_watch_create_exercise_app(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen;
  FAR lv_obj_t *title;
  FAR lv_obj_t *steps_title;
  FAR lv_obj_t *back_btn;
  FAR lv_obj_t *back_lbl;
  FAR lv_obj_t *test_btn;
  FAR lv_obj_t *test_lbl;
  int theme = watch->current_theme;

  screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(theme), 0);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

  title = lv_label_create(screen);
  lv_label_set_text(title, "Exercise");
  lv_obj_set_style_text_color(title, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

  steps_title = lv_label_create(screen);
  lv_label_set_text(steps_title, "Steps today");
  lv_obj_set_style_text_color(steps_title,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(steps_title, &lv_font_montserrat_14, 0);
  lv_obj_align(steps_title, LV_ALIGN_TOP_MID, 0, 62);

  g_ex_app.steps_label = lv_label_create(screen);
  lv_label_set_text(g_ex_app.steps_label, "0");
  lv_obj_set_style_text_color(g_ex_app.steps_label,
                              ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_font(g_ex_app.steps_label,
                             &lv_font_montserrat_24, 0);
  lv_obj_align(g_ex_app.steps_label, LV_ALIGN_TOP_MID, 0, 84);

  g_ex_app.info_label = lv_label_create(screen);
  lv_label_set_text(g_ex_app.info_label, "0 m   " LV_SYMBOL_BULLET
                    "   Rest");
  lv_obj_set_style_text_color(g_ex_app.info_label,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(g_ex_app.info_label,
                             &lv_font_montserrat_16, 0);
  lv_obj_align(g_ex_app.info_label, LV_ALIGN_TOP_MID, 0, 126);

  g_ex_app.status_label = lv_label_create(screen);
  lv_label_set_text(g_ex_app.status_label, "IMU: starting...");
  lv_obj_set_style_text_color(g_ex_app.status_label,
                              lv_color_make(255, 180, 0), 0);
  lv_obj_set_style_text_font(g_ex_app.status_label,
                             &lv_font_montserrat_14, 0);
  lv_obj_align(g_ex_app.status_label, LV_ALIGN_TOP_MID, 0, 162);

  g_ex_app.mag_label = lv_label_create(screen);
  lv_label_set_text(g_ex_app.mag_label, "|a|  -- mg");
  lv_obj_set_style_text_color(g_ex_app.mag_label,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(g_ex_app.mag_label,
                             &lv_font_montserrat_14, 0);
  lv_obj_align(g_ex_app.mag_label, LV_ALIGN_TOP_MID, 0, 196);

  g_ex_app.mag_bar = lv_bar_create(screen);
  lv_obj_set_size(g_ex_app.mag_bar, 330, 14);
  lv_bar_set_range(g_ex_app.mag_bar, 0, 4000);
  lv_bar_set_value(g_ex_app.mag_bar, 0, LV_ANIM_OFF);
  lv_obj_align(g_ex_app.mag_bar, LV_ALIGN_TOP_MID, 0, 224);

  /* Bottom row: Back / Record / Test */

  back_btn = lv_btn_create(screen);
  lv_obj_set_size(back_btn, 90, 45);
  lv_obj_set_style_radius(back_btn, 12, 0);
  lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, 25, -18);
  back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
  lv_obj_center(back_lbl);
  lv_obj_add_event_cb(back_btn, ai_watch_exercise_back_cb,
                      LV_EVENT_CLICKED, watch);

  g_ex_app.rec_btn = lv_btn_create(screen);
  lv_obj_set_size(g_ex_app.rec_btn, 130, 45);
  lv_obj_set_style_radius(g_ex_app.rec_btn, 12, 0);
  lv_obj_set_style_bg_color(g_ex_app.rec_btn,
                            lv_color_make(200, 60, 40), 0);
  lv_obj_align(g_ex_app.rec_btn, LV_ALIGN_BOTTOM_MID, 0, -18);
  g_ex_app.rec_lbl = lv_label_create(g_ex_app.rec_btn);
  lv_label_set_text(g_ex_app.rec_lbl, "Record");
  lv_obj_set_style_text_font(g_ex_app.rec_lbl,
                             &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(g_ex_app.rec_lbl, lv_color_white(), 0);
  lv_obj_center(g_ex_app.rec_lbl);
  lv_obj_add_event_cb(g_ex_app.rec_btn, ai_watch_exercise_rec_cb,
                      LV_EVENT_CLICKED, NULL);

  test_btn = lv_btn_create(screen);
  lv_obj_set_size(test_btn, 90, 45);
  lv_obj_set_style_radius(test_btn, 12, 0);
  lv_obj_set_style_bg_color(test_btn, ai_watch_theme_accent(theme), 0);
  lv_obj_align(test_btn, LV_ALIGN_BOTTOM_RIGHT, -25, -18);
  test_lbl = lv_label_create(test_btn);
  lv_label_set_text(test_lbl, "Test");
  lv_obj_set_style_text_font(test_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(test_lbl, lv_color_white(), 0);
  lv_obj_center(test_lbl);
  lv_obj_add_event_cb(test_btn, ai_watch_exercise_test_cb,
                      LV_EVENT_CLICKED, NULL);

  g_ex_app.last_recording = false;
  g_ex_app.update_timer = lv_timer_create(
      ai_watch_exercise_update_cb, 250, watch);

  watch->app_page_screen = screen;
}

static void ai_watch_destroy_exercise_app(FAR struct ai_watch_s *watch)
{
  if (g_ex_app.update_timer != NULL)
    {
      lv_timer_del(g_ex_app.update_timer);
      g_ex_app.update_timer = NULL;
    }

  memset(&g_ex_app, 0, sizeof(g_ex_app));
  watch->app_page_screen = NULL;
}

/****************************************************************************
 * Private Functions - Fall Alarm Overlay
 *
 * Full-screen modal countdown shown while the motion service has a
 * "suspected fall" pending. Cancelling is possible via the button or
 * KEY2 (the button hook lives in ai_watch_button_update).
 ****************************************************************************/

static void ai_watch_fall_cancel_cb(lv_event_t *e)
{
  (void)e;

  ai_watch_motion_fall_cancel();
}

static void ai_watch_fall_ui_show(void)
{
#if AI_WATCH_CAPTURE_STREAM
  /* Capture build: a 15 s full-screen countdown would interrupt the run.
   * The state machine still prints every decision to the console, which
   * is exactly what the offline replay needs.
   */

  return;
#endif

  if (g_fall_ui.panel == NULL)
    {
      FAR lv_obj_t *panel = lv_obj_create(lv_layer_top());
      FAR lv_obj_t *title;
      FAR lv_obj_t *hint;
      FAR lv_obj_t *btn;
      FAR lv_obj_t *btn_lbl;

      lv_obj_set_size(panel, AI_WATCH_SCREEN_WIDTH,
                      AI_WATCH_SCREEN_HEIGHT);
      lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
      lv_obj_set_style_radius(panel, 0, 0);
      lv_obj_set_style_border_width(panel, 0, 0);
      lv_obj_set_style_bg_color(panel, lv_color_make(120, 12, 12), 0);
      lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
      lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);

      title = lv_label_create(panel);
      lv_label_set_text(title,
                        LV_SYMBOL_WARNING "  Suspected fall");
      lv_obj_set_style_text_color(title, lv_color_white(), 0);
      lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
      lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 96);

      g_fall_ui.cd_label = lv_label_create(panel);
      lv_label_set_text(g_fall_ui.cd_label, "15 s");
      lv_obj_set_style_text_color(g_fall_ui.cd_label,
                                  lv_color_white(), 0);
      lv_obj_set_style_text_font(g_fall_ui.cd_label,
                                 &lv_font_montserrat_24, 0);
      lv_obj_align(g_fall_ui.cd_label, LV_ALIGN_TOP_MID, 0, 150);

      hint = lv_label_create(panel);
      lv_label_set_text(hint, "Tap CANCEL or press KEY2\n"
                        "if you are fine");
      lv_obj_set_style_text_color(hint, lv_color_white(), 0);
      lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 210);

      btn = lv_btn_create(panel);
      lv_obj_set_size(btn, 220, 80);
      lv_obj_set_style_radius(btn, 16, 0);
      lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
      lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -70);
      btn_lbl = lv_label_create(btn);
      lv_label_set_text(btn_lbl, "CANCEL");
      lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_24, 0);
      lv_obj_set_style_text_color(btn_lbl,
                                  lv_color_make(120, 12, 12), 0);
      lv_obj_center(btn_lbl);
      lv_obj_add_event_cb(btn, ai_watch_fall_cancel_cb,
                          LV_EVENT_CLICKED, NULL);

      g_fall_ui.panel = panel;
    }

  lv_obj_clear_flag(g_fall_ui.panel, LV_OBJ_FLAG_HIDDEN);
  g_fall_ui.visible = true;
}

static void ai_watch_fall_ui_hide(void)
{
  if (g_fall_ui.panel != NULL)
    {
      lv_obj_add_flag(g_fall_ui.panel, LV_OBJ_FLAG_HIDDEN);
    }

  g_fall_ui.visible = false;
}

/* Main-loop poll: drives the countdown overlay and converts finished
 * fall events into the standard alert banner (+ the BLE report inside
 * ai_watch_motion_poll_fall).
 */

static void ai_watch_fall_poll(FAR struct ai_watch_s *watch)
{
  struct ai_watch_motion_snapshot_s snap;
  struct ai_watch_fall_report_s rep;
  struct timespec ts;
  uint32_t now_ms;

  ai_watch_motion_get_snapshot(&snap);
  clock_gettime(CLOCK_MONOTONIC, &ts);
  now_ms = (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);

  if (snap.fall_pending && snap.fall_state == FALL_STATE_ALARM)
    {
      int32_t remaining = (int32_t)(snap.fall_deadline_ms - now_ms);
      char buf[16];

      if (remaining < 0)
        {
          remaining = 0;
        }

      if (!g_fall_ui.visible)
        {
          ai_watch_fall_ui_show();
        }

      snprintf(buf, sizeof(buf), "%d s",
               (int)((remaining + 999) / 1000));
      lv_label_set_text(g_fall_ui.cd_label, buf);
    }
  else if (g_fall_ui.visible && !snap.fall_pending)
    {
      ai_watch_fall_ui_hide();
    }

  while (ai_watch_motion_poll_fall(&rep))
    {
      struct ai_watch_ble_alert_s alert;

      memset(&alert, 0, sizeof(alert));
      if (rep.event == AI_WATCH_FALL_CONFIRMED)
        {
          alert.type = AI_WATCH_ALERT_TYPE_FALL;
          strncpy(alert.title, "Report sent to phone",
                  sizeof(alert.title) - 1);
        }
      else if (rep.event == AI_WATCH_FALL_CANCELLED)
        {
          alert.type = AI_WATCH_ALERT_TYPE_FALL_INFO;
          strncpy(alert.title, "Fall alert cancelled",
                  sizeof(alert.title) - 1);
        }
      else
        {
          alert.type = AI_WATCH_ALERT_TYPE_FALL_INFO;
          strncpy(alert.title, "Fall test: flow OK",
                  sizeof(alert.title) - 1);
        }

      ai_watch_alert_show(watch, &alert);
    }
}

/****************************************************************************
 * Private Functions - UI Callbacks
 ****************************************************************************/

/* Segmented theme bar. The tapped segment's sibling index is the theme id,
 * so one callback serves all three without per-segment user data.
 */

static void ai_watch_settings_theme_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);
  FAR lv_obj_t *target = lv_event_get_target(e);
  int index = (int)lv_obj_get_index(target);

  if (index < 0 || index >= AI_WATCH_THEME_COUNT)
    {
      return;
    }

  if (index == watch->current_theme)
    {
      return;
    }

  ai_watch_update_theme(watch, index);
}

static void ai_watch_settings_bt_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);
  bool checked = lv_obj_has_state(watch->bt_switch, LV_STATE_CHECKED);

  ai_watch_ble_set_enabled(checked);
  printf("Settings: Bluetooth switched %s\n", checked ? "ON" : "OFF");
}

/* Set while the code rewrites its own widgets, so a programmatic change is not
 * mistaken for a user action (which would flip auto mode off by accident).
 */

static bool g_settings_sync;

static void ai_watch_settings_show_level(FAR struct ai_watch_s *watch,
                                         int percent)
{
  char buf[8];

  g_settings_sync = true;
  lv_slider_set_value(watch->brightness_slider, percent, LV_ANIM_OFF);
  g_settings_sync = false;

  snprintf(buf, sizeof(buf), "%d%%", percent);
  lv_label_set_text(watch->brightness_value, buf);
}

static void ai_watch_settings_bright_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);
  lv_event_code_t code = lv_event_get_code(e);
  int percent;

  if (code == LV_EVENT_PRESSED)
    {
      /* A horizontal drag on this bar is a level change, not the back-home
       * swipe. The flag is cleared by the touch poller once the finger is
       * lifted, which is after LVGL has finished with the gesture.
       */

      watch->gesture_lock = true;
      return;
    }

  if (g_settings_sync)
    {
      return;
    }

  if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED)
    {
      return;
    }

  percent = (int)lv_slider_get_value(watch->brightness_slider);

  if (code == LV_EVENT_RELEASED)
    {
      printf("Settings: brightness %d%%\n", percent);
      return;
    }

  /* Live preview: the panel follows the knob while it moves. Touching the bar
   * is also the gesture that hands brightness control to the user.
   */

  ai_watch_light_set_level(percent);
  ai_watch_settings_show_level(watch, percent);

  if (lv_obj_has_state(watch->auto_switch, LV_STATE_CHECKED))
    {
      g_settings_sync = true;
      lv_obj_remove_state(watch->auto_switch, LV_STATE_CHECKED);
      g_settings_sync = false;
    }
}

static void ai_watch_settings_auto_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);
  bool checked;

  if (g_settings_sync)
    {
      return;
    }

  checked = lv_obj_has_state(watch->auto_switch, LV_STATE_CHECKED);
  ai_watch_light_set_auto(checked);

  if (checked)
    {
      /* Re-seat the bar on the level the sensor is about to take over. */

      ai_watch_settings_show_level(watch, ai_watch_light_level());
    }

  printf("Settings: auto brightness %s\n", checked ? "ON" : "OFF");
}

/* Keep the bar honest while auto mode is driving the panel: the sensor can
 * change the level behind the UI's back. Skipped while a finger is down so a
 * drag is never fought by a refresh.
 */

static void ai_watch_settings_poll(FAR struct ai_watch_s *watch)
{
  struct timespec now;

  if (watch->current_page != AI_WATCH_PAGE_SETTINGS ||
      watch->brightness_slider == NULL)
    {
      return;
    }

  clock_gettime(CLOCK_MONOTONIC, &now);

  if (!ai_watch_deadline_reached(&now, &watch->next_bright_poll))
    {
      return;
    }

  watch->next_bright_poll = now;
  ai_watch_advance_deadline(&watch->next_bright_poll, 500);

  if (!ai_watch_light_auto() || watch->touch_active)
    {
      return;
    }

  if (ai_watch_light_level() !=
      (int)lv_slider_get_value(watch->brightness_slider))
    {
      ai_watch_settings_show_level(watch, ai_watch_light_level());
    }
}

/* Settings page geometry. The list scrolls, so rows are placed with explicit
 * offsets in the list's content coordinates; with 24 px side padding the
 * usable width is 342 px on the 390 px panel.
 */

#define AI_WATCH_SETTINGS_LIST_Y        46
#define AI_WATCH_SETTINGS_LIST_H        350
#define AI_WATCH_SETTINGS_ROW_BT        6
#define AI_WATCH_SETTINGS_ROW_BT_STATE  42
#define AI_WATCH_SETTINGS_ROW_THEME     88
#define AI_WATCH_SETTINGS_BAR_THEME     122
#define AI_WATCH_SETTINGS_ROW_BRIGHT    190
#define AI_WATCH_SETTINGS_BAR_BRIGHT    224
#define AI_WATCH_SETTINGS_ROW_AUTO      306
#define AI_WATCH_SETTINGS_ROW_ABOUT     366
#define AI_WATCH_SETTINGS_BAR_H         44
#define AI_WATCH_SETTINGS_SEG_W         110
#define AI_WATCH_SETTINGS_SEG_STEP      116

static void ai_watch_create_settings_page(FAR struct ai_watch_s *watch)
{
  static FAR const char *const theme_names[AI_WATCH_THEME_COUNT] =
  {
    "Dark", "Light", "Blue"
  };

  FAR lv_obj_t *screen = lv_obj_create(NULL);
  FAR lv_obj_t *title;
  FAR lv_obj_t *list;
  FAR lv_obj_t *bt_label;
  FAR lv_obj_t *theme_label;
  FAR lv_obj_t *brightness_label;
  FAR lv_obj_t *auto_label;
  FAR lv_obj_t *hint;
  char buf[8];
  int theme = watch->current_theme;
  int i;

  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(theme), 0);
  watch->settings_screen = screen;
  watch->fixed_pages[AI_WATCH_PAGE_SETTINGS] = screen;

  title = lv_label_create(screen);
  lv_label_set_text(title, "Settings");
  lv_obj_set_style_text_color(title, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  /* Scrollable column: the page no longer has to fit into one 450 px screen */

  list = lv_obj_create(screen);
  watch->settings_list = list;
  lv_obj_set_size(list, LV_PCT(100), AI_WATCH_SETTINGS_LIST_H);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, AI_WATCH_SETTINGS_LIST_Y);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_radius(list, 0, 0);
  lv_obj_set_style_pad_hor(list, 24, 0);
  lv_obj_set_style_pad_top(list, 0, 0);
  lv_obj_set_style_pad_bottom(list, 16, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_width(list, 5, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(list, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(list, ai_watch_theme_secondary(theme),
                            LV_PART_SCROLLBAR);

  bt_label = lv_label_create(list);
  lv_label_set_text(bt_label, "Bluetooth");
  lv_obj_set_style_text_color(bt_label, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(bt_label, &lv_font_montserrat_20, 0);
  lv_obj_align(bt_label, LV_ALIGN_TOP_LEFT, 0, AI_WATCH_SETTINGS_ROW_BT);

  /* Bluetooth switch - wired to the real BLE bridge; advertising and
   * connections stop when it is off.
   */

  watch->bt_switch = lv_switch_create(list);
  lv_obj_add_state(watch->bt_switch, LV_STATE_CHECKED);
  lv_obj_add_event_cb(watch->bt_switch, ai_watch_settings_bt_cb,
                      LV_EVENT_VALUE_CHANGED, watch);
  lv_obj_align(watch->bt_switch, LV_ALIGN_TOP_RIGHT, 0,
               AI_WATCH_SETTINGS_ROW_BT - 4);

  watch->ble_state_label = lv_label_create(list);
  lv_label_set_text(watch->ble_state_label,
                    ai_watch_ble_get_status_text(ai_watch_ble_get_state()));
  lv_obj_set_style_text_color(watch->ble_state_label,
                              ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_font(watch->ble_state_label,
                             &lv_font_montserrat_14, 0);
  lv_obj_align(watch->ble_state_label, LV_ALIGN_TOP_RIGHT, 0,
               AI_WATCH_SETTINGS_ROW_BT_STATE);

  theme_label = lv_label_create(list);
  lv_label_set_text(theme_label, "Theme");
  lv_obj_set_style_text_color(theme_label, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(theme_label, &lv_font_montserrat_20, 0);
  lv_obj_align(theme_label, LV_ALIGN_TOP_LEFT, 0, AI_WATCH_SETTINGS_ROW_THEME);

  /* Three equal segments in one rounded bar, selected one filled with the
   * accent colour. Painted by ai_watch_settings_bar_paint().
   */

  watch->theme_seg_bar = lv_obj_create(list);
  lv_obj_set_size(watch->theme_seg_bar, LV_PCT(100), AI_WATCH_SETTINGS_BAR_H);
  lv_obj_align(watch->theme_seg_bar, LV_ALIGN_TOP_LEFT, 0,
               AI_WATCH_SETTINGS_BAR_THEME);
  lv_obj_set_style_bg_opa(watch->theme_seg_bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(watch->theme_seg_bar, 0, 0);
  lv_obj_set_style_pad_all(watch->theme_seg_bar, 0, 0);
  lv_obj_clear_flag(watch->theme_seg_bar, LV_OBJ_FLAG_SCROLLABLE);

  for (i = 0; i < AI_WATCH_THEME_COUNT; i++)
    {
      FAR lv_obj_t *seg = lv_obj_create(watch->theme_seg_bar);
      FAR lv_obj_t *seg_text;

      watch->theme_seg[i] = seg;

      lv_obj_set_size(seg, AI_WATCH_SETTINGS_SEG_W, AI_WATCH_SETTINGS_BAR_H);
      lv_obj_align(seg, LV_ALIGN_TOP_LEFT, i * AI_WATCH_SETTINGS_SEG_STEP, 0);
      lv_obj_set_style_radius(seg, AI_WATCH_SETTINGS_BAR_H / 2, 0);
      lv_obj_set_style_border_width(seg, 0, 0);
      lv_obj_set_style_pad_all(seg, 0, 0);
      lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_event_cb(seg, ai_watch_settings_theme_cb,
                          LV_EVENT_CLICKED, watch);

      seg_text = lv_label_create(seg);
      watch->theme_seg_label[i] = seg_text;
      lv_label_set_text(seg_text, theme_names[i]);
      lv_obj_set_style_text_font(seg_text, &lv_font_montserrat_16, 0);
      lv_obj_center(seg_text);
    }

  brightness_label = lv_label_create(list);
  lv_label_set_text(brightness_label, "Brightness");
  lv_obj_set_style_text_color(brightness_label,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(brightness_label, &lv_font_montserrat_20, 0);
  lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 0,
               AI_WATCH_SETTINGS_ROW_BRIGHT);

  watch->brightness_value = lv_label_create(list);
  snprintf(buf, sizeof(buf), "%d%%", ai_watch_light_level());
  lv_label_set_text(watch->brightness_value, buf);
  lv_obj_set_style_text_font(watch->brightness_value,
                             &lv_font_montserrat_20, 0);
  lv_obj_align(watch->brightness_value, LV_ALIGN_TOP_RIGHT, 0,
               AI_WATCH_SETTINGS_ROW_BRIGHT);

  /* One continuous 1..100 bar: dragging previews the change live, tapping
   * jumps straight to that level. Both take brightness away from the sensor.
   */

  watch->brightness_slider = lv_slider_create(list);
  lv_obj_set_size(watch->brightness_slider, LV_PCT(100),
                  AI_WATCH_SETTINGS_BAR_H);
  lv_obj_align(watch->brightness_slider, LV_ALIGN_TOP_LEFT, 0,
               AI_WATCH_SETTINGS_BAR_BRIGHT);
  lv_slider_set_range(watch->brightness_slider, 1, 100);
  lv_slider_set_value(watch->brightness_slider, ai_watch_light_level(),
                      LV_ANIM_OFF);
  lv_obj_set_style_radius(watch->brightness_slider,
                          AI_WATCH_SETTINGS_BAR_H / 2, LV_PART_MAIN);
  lv_obj_set_style_radius(watch->brightness_slider,
                          AI_WATCH_SETTINGS_BAR_H / 2, LV_PART_INDICATOR);
  lv_obj_set_style_border_width(watch->brightness_slider, 3, LV_PART_KNOB);
  lv_obj_add_event_cb(watch->brightness_slider,
                      ai_watch_settings_bright_cb, LV_EVENT_ALL, watch);

  auto_label = lv_label_create(list);
  lv_label_set_text(auto_label, "Auto brightness");
  lv_obj_set_style_text_color(auto_label, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(auto_label, &lv_font_montserrat_20, 0);
  lv_obj_align(auto_label, LV_ALIGN_TOP_LEFT, 0, AI_WATCH_SETTINGS_ROW_AUTO);

  watch->auto_switch = lv_switch_create(list);
  if (ai_watch_light_auto())
    {
      lv_obj_add_state(watch->auto_switch, LV_STATE_CHECKED);
    }

  lv_obj_add_event_cb(watch->auto_switch, ai_watch_settings_auto_cb,
                      LV_EVENT_VALUE_CHANGED, watch);
  lv_obj_align(watch->auto_switch, LV_ALIGN_TOP_RIGHT, 0,
               AI_WATCH_SETTINGS_ROW_AUTO - 4);

  watch->about_label = lv_label_create(list);
  lv_label_set_text(watch->about_label,
                    "AI Watch v" AI_WATCH_VERSION "\n"
                    "Board: SF32LB52\n"
                    "Touch: Checking...\n"
                    "RTC: Checking...");
  lv_obj_set_style_text_color(watch->about_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(watch->about_label, &lv_font_montserrat_16, 0);
  lv_obj_align(watch->about_label, LV_ALIGN_TOP_LEFT, 0,
               AI_WATCH_SETTINGS_ROW_ABOUT);

  /* Fixed footer: the title and these hints stay put while the list scrolls */

  hint = lv_label_create(screen);
  lv_label_set_text(hint, LV_SYMBOL_LEFT " Back (KEY2)   "
                           LV_SYMBOL_UP LV_SYMBOL_DOWN " Scroll");
  lv_obj_set_style_text_color(hint, ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

  ai_watch_settings_bar_paint(watch, theme);

  clock_gettime(CLOCK_MONOTONIC, &watch->next_bright_poll);
}

/****************************************************************************
 * Private Functions - Brief Page ("AI negative screen")
 *
 * The page reachable by swiping left from the watchface. Everything on it
 * is either local (counters from the motion service, pending reminders
 * from the BLE store) or synced from the phone (weather and one sentence
 * of commentary). With no phone the local half still renders, so the page
 * is never empty.
 ****************************************************************************/

struct ai_watch_brief_ui_s
{
  FAR lv_obj_t *screen;
  FAR lv_obj_t *steps_val;
  FAR lv_obj_t *active_val;
  FAR lv_obj_t *falls_val;
  FAR lv_obj_t *weather_val;
  FAR lv_obj_t *comment_val;
  FAR lv_obj_t *reminder_box;
  FAR lv_obj_t *empty_hint;
};

static struct ai_watch_brief_ui_s g_brief_ui;

static FAR const char *ai_watch_weather_text(uint8_t code)
{
  switch (code)
    {
      case AI_WATCH_WEATHER_CLEAR:    return "晴";
      case AI_WATCH_WEATHER_CLOUDY:   return "多云";
      case AI_WATCH_WEATHER_OVERCAST: return "阴";
      case AI_WATCH_WEATHER_RAIN:     return "雨";
      case AI_WATCH_WEATHER_SNOW:     return "雪";
      case AI_WATCH_WEATHER_FOG:      return "雾";
      default:                        return NULL;
    }
}

/* Rebuild the reminder lines. Only future-dated ones count as "not yet
 * fired"; the ones already alerted live in the Reminder app instead.
 */

#define AI_WATCH_BRIEF_REMINDERS_MAX 3

static void ai_watch_brief_refresh_reminders(FAR struct ai_watch_s *watch,
                                             int theme)
{
  FAR struct ai_watch_reminder_store_s *store = ai_watch_ble_get_reminders();
  time_t now = time(NULL);
  int shown = 0;
  int i;

  if (g_brief_ui.reminder_box == NULL)
    {
      return;
    }

  lv_obj_clean(g_brief_ui.reminder_box);

  for (i = 0; i < AI_WATCH_REMINDER_MAX && shown < AI_WATCH_BRIEF_REMINDERS_MAX;
       i++)
    {
      FAR struct ai_watch_reminder_s *r = &store->items[i];
      FAR lv_obj_t *row;

      if (r->id == 0 || r->due_alerted)
        {
          continue;
        }

      if (r->timestamp != 0 && (time_t)r->timestamp <= now)
        {
          continue;
        }

      row = lv_label_create(g_brief_ui.reminder_box);
      lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
      lv_obj_set_width(row, AI_WATCH_BRIEF_WIDTH - 48);
      lv_label_set_text_fmt(row, LV_SYMBOL_BELL " %s", r->title);
      lv_obj_set_style_text_font(row, &ai_watch_font_cjk_16, 0);
      lv_obj_set_style_text_color(row, ai_watch_theme_text(theme), 0);
      shown++;
    }

  if (shown == 0)
    {
      FAR lv_obj_t *row = lv_label_create(g_brief_ui.reminder_box);

      lv_label_set_text(row, "暂无待办提醒");
      lv_obj_set_style_text_font(row, &ai_watch_font_cjk_16, 0);
      lv_obj_set_style_text_color(row, ai_watch_theme_secondary(theme), 0);
    }
}

static void ai_watch_brief_refresh(FAR struct ai_watch_s *watch)
{
  struct ai_watch_motion_snapshot_s snap;
  FAR struct ai_watch_brief_s *b = ai_watch_ble_get_brief();
  int theme = watch->current_theme;
  char buf[64];

  if (g_brief_ui.screen == NULL)
    {
      return;
    }

  ai_watch_motion_get_snapshot(&snap);

  /* Local counters win: they are live, the phone's copy is a snapshot
   * taken when it built the brief.
   */

  if (g_brief_ui.steps_val != NULL)
    {
      snprintf(buf, sizeof(buf), "%lu", (unsigned long)snap.steps_today);
      lv_label_set_text(g_brief_ui.steps_val, buf);
    }

  if (g_brief_ui.active_val != NULL)
    {
      snprintf(buf, sizeof(buf), "%lu", (unsigned long)(snap.active_ms_today / 60000u));
      lv_label_set_text(g_brief_ui.active_val, buf);
    }

  if (g_brief_ui.falls_val != NULL)
    {
      snprintf(buf, sizeof(buf), "%u", (unsigned)snap.fall_today);
      lv_label_set_text(g_brief_ui.falls_val, buf);
    }

  if (g_brief_ui.weather_val != NULL)
    {
      FAR const char *w = ai_watch_weather_text(b->weather);

      if (b->valid && w != NULL)
        {
          /* ℃ is one glyph (U+2103, from DejaVu in the weather font);
           * "%d°C" rendered an invisible ring because Droid Sans
           * Fallback has no U+00B0. */

          snprintf(buf, sizeof(buf), "%s %d℃", w, (int)b->temp_c);
          lv_label_set_text(g_brief_ui.weather_val, buf);
          lv_obj_set_style_text_font(g_brief_ui.weather_val,
                                     &ai_watch_font_weather_28, 0);
          lv_obj_set_style_text_color(g_brief_ui.weather_val,
                                      ai_watch_theme_text(theme), 0);
        }
      else
        {
          lv_label_set_text(g_brief_ui.weather_val,
                            "连接手机后显示天气");
          lv_obj_set_style_text_font(g_brief_ui.weather_val,
                                     &ai_watch_font_cjk_16, 0);
          lv_obj_set_style_text_color(g_brief_ui.weather_val,
                                      ai_watch_theme_secondary(theme), 0);
        }
    }

  if (g_brief_ui.comment_val != NULL)
    {
      if (b->valid && b->text[0] != '\0')
        {
          lv_label_set_text(g_brief_ui.comment_val, b->text);
        }
      else
        {
          lv_label_set_text(g_brief_ui.comment_val,
                            "连接手机后可生成 AI 点评");
        }
    }

  ai_watch_brief_refresh_reminders(watch, theme);

  b->pending = false;
}

static void ai_watch_create_brief_page(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen = lv_obj_create(NULL);
  FAR lv_obj_t *title;
  FAR lv_obj_t *hint;
  FAR lv_obj_t *stats;
  FAR lv_obj_t *col;
  FAR lv_obj_t *bar;
  FAR lv_obj_t *card;
  int theme = watch->current_theme;

  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(theme), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  watch->fixed_pages[AI_WATCH_PAGE_BRIEF] = screen;
  g_brief_ui.screen = screen;

  /* ---- Header: title + accent underline ---- */

  title = lv_label_create(screen);
  lv_label_set_text(title, "每日简报");
  lv_obj_set_style_text_color(title, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(title, &ai_watch_font_cjk_16, 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 14);

  bar = lv_obj_create(screen);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, 40, 3);
  lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 20, 44);
  lv_obj_set_style_bg_color(bar, ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

  /* Content now reaches ~505 px on the 450 px panel: vertical scroll
   * reveals the lower cards. Page navigation on this screen is
   * horizontal-only, so a vertical drag scrolls instead of switching. */

  lv_obj_add_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(screen, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

  /* ---- Weather card: ONE label styled as the card. The container
   * variant (lv_obj + 2 labels) was observed to wedge BLE bring-up on
   * every boot (bt_enable -110) and is documented in the dev log --
   * object count on this page stays identical to the 4.8.55 layout. */

  g_brief_ui.weather_val = lv_label_create(screen);
  lv_label_set_text(g_brief_ui.weather_val, "连接手机后显示天气");
  lv_obj_set_size(g_brief_ui.weather_val, AI_WATCH_BRIEF_WIDTH - 32, 64);
  lv_obj_align(g_brief_ui.weather_val, LV_ALIGN_TOP_MID, 0, 58);
  lv_obj_set_style_bg_color(g_brief_ui.weather_val,
                            ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_bg_opa(g_brief_ui.weather_val, LV_OPA_30, 0);
  lv_obj_set_style_border_width(g_brief_ui.weather_val, 1, 0);
  lv_obj_set_style_border_color(g_brief_ui.weather_val,
                                ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_border_opa(g_brief_ui.weather_val, LV_OPA_50, 0);
  lv_obj_set_style_radius(g_brief_ui.weather_val, 14, 0);
  lv_obj_set_style_pad_left(g_brief_ui.weather_val, 12, 0);
  lv_obj_set_style_text_color(g_brief_ui.weather_val,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(g_brief_ui.weather_val,
                             &ai_watch_font_cjk_16, 0);

  /* ---- Stats card: three counters on a subtle panel ---- */

  stats = lv_obj_create(screen);
  lv_obj_set_size(stats, AI_WATCH_BRIEF_WIDTH - 32, 100);
  lv_obj_align(stats, LV_ALIGN_TOP_MID, 0, 134);
  lv_obj_set_style_bg_color(stats, ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_bg_opa(stats, LV_OPA_30, 0);
  lv_obj_set_style_border_width(stats, 1, 0);
  lv_obj_set_style_border_color(stats, ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_border_opa(stats, LV_OPA_50, 0);
  lv_obj_set_style_radius(stats, 14, 0);
  lv_obj_set_style_pad_all(stats, 4, 0);
  lv_obj_clear_flag(stats, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  {
    static const char *names[3] = { "步数", "活动", "跌倒" };
    FAR lv_obj_t **slots[3] =
      {
        &g_brief_ui.steps_val,
        &g_brief_ui.active_val,
        &g_brief_ui.falls_val
      };
    int k;

    for (k = 0; k < 3; k++)
      {
        FAR lv_obj_t *unit;

        col = lv_obj_create(stats);
        lv_obj_set_size(col, (AI_WATCH_BRIEF_WIDTH - 40) / 3, 88);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 0, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        *slots[k] = lv_label_create(col);
        lv_label_set_text(*slots[k], "--");
        lv_obj_set_style_text_color(*slots[k], ai_watch_theme_accent(theme), 0);
        lv_obj_set_style_text_font(*slots[k], &lv_font_montserrat_28, 0);

        unit = lv_label_create(col);
        lv_label_set_text(unit, names[k]);
        lv_obj_set_style_text_color(unit, ai_watch_theme_secondary(theme), 0);
        lv_obj_set_style_text_font(unit, &ai_watch_font_cjk_16, 0);
      }
  }

  /* ---- Pending reminders ---- */

  {
    FAR lv_obj_t *head = lv_label_create(screen);

    lv_label_set_text(head, "待办");
    lv_obj_set_style_text_color(head, ai_watch_theme_secondary(theme), 0);
    lv_obj_set_style_text_font(head, &ai_watch_font_cjk_16, 0);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 20, 250);
  }

  g_brief_ui.reminder_box = lv_obj_create(screen);
  lv_obj_set_size(g_brief_ui.reminder_box, AI_WATCH_BRIEF_WIDTH - 32, 92);
  lv_obj_align(g_brief_ui.reminder_box, LV_ALIGN_TOP_MID, 0, 274);
  lv_obj_set_style_bg_color(g_brief_ui.reminder_box,
                            ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_bg_opa(g_brief_ui.reminder_box, LV_OPA_30, 0);
  lv_obj_set_style_border_width(g_brief_ui.reminder_box, 1, 0);
  lv_obj_set_style_border_color(g_brief_ui.reminder_box,
                                ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_border_opa(g_brief_ui.reminder_box, LV_OPA_50, 0);
  lv_obj_set_style_radius(g_brief_ui.reminder_box, 12, 0);
  lv_obj_set_style_pad_all(g_brief_ui.reminder_box, 8, 0);
  lv_obj_set_style_pad_row(g_brief_ui.reminder_box, 6, 0);
  lv_obj_set_flex_flow(g_brief_ui.reminder_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_brief_ui.reminder_box, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_clear_flag(g_brief_ui.reminder_box, LV_OBJ_FLAG_SCROLLABLE);

  /* ---- AI commentary card: accent bar + wrapped text ---- */

  card = lv_obj_create(screen);
  lv_obj_set_size(card, AI_WATCH_BRIEF_WIDTH - 32, 92);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 382);
  lv_obj_set_style_bg_color(card, ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_30, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  bar = lv_obj_create(card);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, 3, 68);
  lv_obj_align(bar, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_set_style_bg_color(bar, ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

  g_brief_ui.comment_val = lv_label_create(card);
  lv_label_set_long_mode(g_brief_ui.comment_val, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_brief_ui.comment_val, AI_WATCH_BRIEF_WIDTH - 72);
  lv_label_set_text(g_brief_ui.comment_val, "连接手机后可生成 AI 点评");
  lv_obj_set_style_text_color(g_brief_ui.comment_val,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(g_brief_ui.comment_val,
                             &ai_watch_font_cjk_16, 0);
  lv_obj_align(g_brief_ui.comment_val, LV_ALIGN_LEFT_MID, 16, 0);

  hint = lv_label_create(screen);
  lv_label_set_text(hint, "返回 (KEY2)");
  lv_obj_set_style_text_color(hint, ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(hint, &ai_watch_font_cjk_16, 0);
  /* Below the comment card (ends at y=474): BOTTOM_MID would clamp to
   * the 450 px panel and land inside the card once content overflows. */
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 486);
}

/****************************************************************************
 * Private Functions - Home Page (Digital Watchface)
 ****************************************************************************/

static void ai_watch_create_home_page(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen = lv_obj_create(NULL);
  FAR lv_obj_t *time_row;
  int theme = watch->current_theme;

  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(theme), 0);
  watch->home_screen = screen;
  watch->fixed_pages[AI_WATCH_PAGE_HOME] = screen;

  /* Title */

  watch->home_title_label = lv_label_create(screen);
  lv_label_set_text(watch->home_title_label, "AI Watch");
  lv_obj_set_style_text_color(watch->home_title_label,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(watch->home_title_label,
                             &lv_font_montserrat_20, 0);
  lv_obj_align(watch->home_title_label, LV_ALIGN_TOP_MID, 0, 26);

  /* Date + weekday combined in one label so the two can never overlap */

  watch->date_label = lv_label_create(screen);
  lv_label_set_text(watch->date_label, "----/--/--");
  lv_obj_set_style_text_color(watch->date_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(watch->date_label,
                             &lv_font_montserrat_16, 0);
  lv_obj_align(watch->date_label, LV_ALIGN_TOP_MID, 0, 58);

  /* Time row: HH:MM (48 px) and the seconds (28 px, no colon) are laid
   * out by a fixed-gap flex container, so a size change on either side
   * re-flows the row instead of overlapping the neighbor.
   */

  time_row = lv_obj_create(screen);
  lv_obj_set_size(time_row, AI_WATCH_SCREEN_WIDTH, 64);
  lv_obj_align(time_row, LV_ALIGN_CENTER, 0, -38);
  lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(time_row, 0, 0);
  lv_obj_set_style_pad_all(time_row, 0, 0);
  lv_obj_set_style_pad_column(time_row, 10, 0);
  lv_obj_set_scrollbar_mode(time_row, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(time_row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  watch->time_label = lv_label_create(time_row);
  lv_label_set_text(watch->time_label, "--:--");
  lv_obj_set_style_text_color(watch->time_label,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(watch->time_label, &lv_font_montserrat_48, 0);

  watch->seconds_label = lv_label_create(time_row);
  lv_label_set_text(watch->seconds_label, "--");
  lv_obj_set_style_text_color(watch->seconds_label,
                              ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_font(watch->seconds_label,
                             &lv_font_montserrat_28, 0);

  /* Unread reminder count (hidden while there is nothing unread) */

  watch->unread_label = lv_label_create(screen);
  lv_label_set_text(watch->unread_label, "");
  lv_obj_set_style_text_color(watch->unread_label,
                              ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_font(watch->unread_label,
                             &lv_font_montserrat_16, 0);
  lv_obj_align(watch->unread_label, LV_ALIGN_CENTER, 0, 20);
  lv_obj_add_flag(watch->unread_label, LV_OBJ_FLAG_HIDDEN);

  /* Bluetooth status */

  watch->bt_label = lv_label_create(screen);
  lv_label_set_text(watch->bt_label, "BT: OFF");
  lv_obj_set_style_text_color(watch->bt_label,
                              ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_font(watch->bt_label, &lv_font_montserrat_20, 0);
  lv_obj_align(watch->bt_label, LV_ALIGN_BOTTOM_MID, 0, -64);

  /* Battery gauge: drawn from primitives because this build's Montserrat
   * faces carry no LV_SYMBOL_BATTERY_* glyphs. Shell + fill + percentage.
   */

  watch->battery_shell = lv_obj_create(screen);
  lv_obj_set_size(watch->battery_shell, 26, 13);
  lv_obj_align(watch->battery_shell, LV_ALIGN_TOP_RIGHT, -66, 31);
  lv_obj_set_style_bg_opa(watch->battery_shell, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(watch->battery_shell, 2, 0);
  lv_obj_set_style_border_color(watch->battery_shell,
                                ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_radius(watch->battery_shell, 3, 0);
  lv_obj_set_style_pad_all(watch->battery_shell, 0, 0);
  lv_obj_clear_flag(watch->battery_shell, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(watch->battery_shell, LV_OBJ_FLAG_CLICKABLE);

  watch->battery_fill = lv_obj_create(watch->battery_shell);
  lv_obj_set_size(watch->battery_fill, 4, 7);
  lv_obj_align(watch->battery_fill, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_bg_color(watch->battery_fill,
                            ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_bg_opa(watch->battery_fill, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(watch->battery_fill, 0, 0);
  lv_obj_set_style_radius(watch->battery_fill, 1, 0);
  lv_obj_set_style_pad_all(watch->battery_fill, 0, 0);
  lv_obj_clear_flag(watch->battery_fill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(watch->battery_fill, LV_OBJ_FLAG_CLICKABLE);

  watch->battery_label = lv_label_create(screen);
  lv_label_set_text(watch->battery_label, "--%");
  lv_obj_set_style_text_color(watch->battery_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(watch->battery_label,
                             &lv_font_montserrat_14, 0);
  lv_obj_align(watch->battery_label, LV_ALIGN_TOP_RIGHT, -16, 27);

  /* 0% is a legal reading, so the "already shown" sentinel must not be 0. */

  watch->battery_shown_pct = -2;

  /* Gesture hint */

  watch->home_hint_label = lv_label_create(screen);
  lv_label_set_text(watch->home_hint_label, "Swipe up for apps");
  lv_obj_set_style_text_color(watch->home_hint_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(watch->home_hint_label,
                             &lv_font_montserrat_14, 0);
  lv_obj_align(watch->home_hint_label, LV_ALIGN_BOTTOM_MID, 0, -24);

  /* The CLICKED-to-open-the-app-list callback was removed 2026-11-01:
   * over the remote preview a swipe that ends on the screen object fires
   * CLICKED (no scrolling on a fixed screen), so every swipe degraded
   * into "tap opens the app list". The list is swipe-up only now. */
}

/****************************************************************************
 * Private Functions - App List Page (Hex Ring Menu)
 ****************************************************************************/

static void ai_watch_create_app_list_page(FAR struct ai_watch_s *watch)
{
  FAR struct ai_menu_ctx_s *ctx = &watch->menu;
  FAR lv_obj_t *screen;
  int theme = watch->current_theme;
  int app_count = (int)AI_WATCH_APP_COUNT;
  int max_icons;
  int i;

  /* Create screen */

  screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(theme), 0);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
  watch->app_list_screen = screen;
  watch->fixed_pages[AI_WATCH_PAGE_APP_LIST] = screen;

  /* Initialize menu context */

  memset(ctx, 0, sizeof(*ctx));
  ctx->focused_index = 0;
  ctx->scroll_x = 0.0f;
  ctx->scroll_y = 0.0f;

  /* Create a transparent container covering the full screen.
   * This is the touch target for drag events.
   */

  ctx->container = lv_obj_create(screen);
  lv_obj_set_size(ctx->container, AI_WATCH_SCREEN_WIDTH,
                  AI_WATCH_SCREEN_HEIGHT);
  lv_obj_set_style_bg_opa(ctx->container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ctx->container, 0, 0);
  lv_obj_set_style_pad_all(ctx->container, 0, 0);
  lv_obj_set_scrollbar_mode(ctx->container, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(ctx->container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(ctx->container, ai_menu_event_cb,
                      LV_EVENT_PRESSED, watch);
  lv_obj_add_event_cb(ctx->container, ai_menu_event_cb,
                      LV_EVENT_PRESSING, watch);
  lv_obj_add_event_cb(ctx->container, ai_menu_event_cb,
                      LV_EVENT_RELEASED, watch);
  lv_obj_add_event_cb(ctx->container, ai_menu_event_cb,
                      LV_EVENT_PRESS_LOST, watch);

  /* Place app icons in hex ring layout (1 + 6 + 12) */

  max_icons = (app_count < AI_WATCH_MENU_MAX_ICONS) ?
              app_count : AI_WATCH_MENU_MAX_ICONS;
  ctx->icon_count = max_icons;

  for (i = 0; i < max_icons; i++)
    {
      FAR const struct ai_app_desc_s *app = &g_app_registry[i];
      FAR lv_obj_t *img;
      float hx;
      float hy;

      /* Compute hex position */

      ai_menu_hex_position(i, &hx, &hy);

      ctx->icons[i].base_x = hx;
      ctx->icons[i].base_y = hy;
      ctx->icons[i].app_index = i;
      ctx->icons[i].last_x = -9999;
      ctx->icons[i].last_y = -9999;
      ctx->icons[i].last_tier = -1;   /* starts hidden */
      ctx->icons[i].last_opa = 0;

      /* Create LVGL image object — starts hidden until first transform */

      img = lv_img_create(screen);
      lv_img_set_src(img, app->icon_tier[1]);  /* default: 100px tier */
      lv_obj_set_style_opa(img, LV_OPA_TRANSP, 0);
      lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);

      ctx->icons[i].img_obj = img;
    }

  /* Initial transform */

  ai_menu_update_transform(watch);
}

/****************************************************************************
 * Private Functions - AI Assistant App
 *
 * One app page: a Start button tells the phone to run a voice/AI round
 * (DataUpload f3 AI_TRIGGER), the AI answer arrives as an AI_TEXT
 * command and is shown here (a truncated copy pops the alert banner).
 * An AI_TIMER command starts a countdown that keeps running with the
 * page closed and fires a banner when it expires.
 ****************************************************************************/

#define AI_WATCH_AI_STATUS_TIMER_MS  500
#define AI_WATCH_AI_WAIT_TIMEOUT_MS  60000

struct ai_ai_app_s
{
  /* UI objects (valid only while the page is open) */

  FAR lv_obj_t *status_label;
  FAR lv_obj_t *result_label;
  FAR lv_obj_t *timer_label;
  bool waiting;                     /* trigger sent, reply pending */
  uint32_t wait_sent_tick;
  FAR lv_timer_t *update_timer;

  /* AI countdown - survives page close/reopen */

  bool cd_running;
  uint32_t cd_total_s;
  char cd_label[AI_WATCH_REMINDER_TITLE_MAX + 1];
  struct timespec cd_deadline;
};

static struct ai_ai_app_s g_ai_app;

static void ai_watch_ai_back_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);

  ai_watch_pop_page(watch);
}

static void ai_watch_ai_start_cb(lv_event_t *e)
{
  int ret;

  (void)e;

  if (ai_watch_ble_get_state() != AI_WATCH_BLE_BSP_CONNECTED)
    {
      g_ai_app.waiting = false;
      lv_label_set_text(g_ai_app.status_label, "BLE not connected");
      return;
    }

  ret = ai_watch_ble_send_ai_trigger(AI_WATCH_AI_TRIGGER_SRC_PAGE,
                                     NULL, 0);
  if (ret < 0)
    {
      g_ai_app.waiting = false;
      lv_label_set_text(g_ai_app.status_label, "Send failed");
      printf("AI: trigger send failed: %d\n", ret);
      return;
    }

  g_ai_app.waiting = true;
  g_ai_app.wait_sent_tick = lv_tick_get();
  lv_label_set_text(g_ai_app.status_label, "Listening on phone...");
}

static void ai_watch_ai_update_cb(FAR lv_timer_t *timer)
{
  FAR struct ai_watch_s *watch = lv_timer_get_user_data(timer);
  FAR struct ai_watch_ai_result_s *res = ai_watch_ble_get_ai_result();
  enum ai_watch_ble_bsp_state_e state = ai_watch_ble_get_state();
  int theme = watch->current_theme;
  char buf[48];

  /* New AI result from the phone */

  if (res->pending)
    {
      res->pending = false;
      g_ai_app.waiting = false;
      lv_obj_set_style_text_font(g_ai_app.result_label,
                                 ai_watch_text_is_ascii(res->text) ?
                                 &lv_font_montserrat_16 :
                                 &ai_watch_font_cjk_16, 0);
      lv_label_set_text(g_ai_app.result_label, res->text);
    }

  /* Status line */

  if (g_ai_app.waiting &&
      lv_tick_elaps(g_ai_app.wait_sent_tick) <
      AI_WATCH_AI_WAIT_TIMEOUT_MS)
    {
      /* Keep the listening hint */
    }
  else
    {
      if (g_ai_app.waiting)
        {
          g_ai_app.waiting = false;
        }

      if (state != AI_WATCH_BLE_BSP_CONNECTED)
        {
          lv_obj_set_style_text_color(g_ai_app.status_label,
                                      ai_watch_theme_secondary(theme), 0);
          lv_label_set_text(g_ai_app.status_label,
                            ai_watch_ble_get_status_text(state));
        }
      else
        {
          lv_obj_set_style_text_color(g_ai_app.status_label,
                                      ai_watch_theme_accent(theme), 0);
          lv_label_set_text(g_ai_app.status_label,
                            "Connected - press Start");
        }
    }

  /* Countdown line (ascii only; the label shows in the expiry banner) */

  if (g_ai_app.cd_running)
    {
      struct timespec now;
      uint32_t remain;
      uint32_t hours;
      uint32_t mins;
      uint32_t secs;

      clock_gettime(CLOCK_MONOTONIC, &now);

      if (now.tv_sec < g_ai_app.cd_deadline.tv_sec)
        {
          remain = (uint32_t)(g_ai_app.cd_deadline.tv_sec - now.tv_sec);
        }
      else
        {
          remain = 0;
        }

      hours = remain / 3600;
      mins = (remain % 3600) / 60;
      secs = remain % 60;

      if (hours != 0)
        {
          snprintf(buf, sizeof(buf), "Timer %lu:%02lu:%02lu",
                   (unsigned long)hours, (unsigned long)mins,
                   (unsigned long)secs);
        }
      else
        {
          snprintf(buf, sizeof(buf), "Timer %02lu:%02lu",
                   (unsigned long)mins, (unsigned long)secs);
        }

      lv_obj_set_style_text_color(g_ai_app.timer_label,
                                  ai_watch_theme_accent(theme), 0);
      lv_label_set_text(g_ai_app.timer_label, buf);
    }
  else
    {
      lv_label_set_text(g_ai_app.timer_label, "");
    }
}

static void ai_watch_create_ai_app(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen;
  FAR lv_obj_t *title;
  FAR lv_obj_t *back_btn;
  FAR lv_obj_t *back_lbl;
  FAR lv_obj_t *start_btn;
  FAR lv_obj_t *start_lbl;
  int theme = watch->current_theme;

  screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(theme), 0);
  lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

  title = lv_label_create(screen);
  lv_label_set_text(title, "AI Assistant");
  lv_obj_set_style_text_color(title, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  g_ai_app.status_label = lv_label_create(screen);
  lv_label_set_text(g_ai_app.status_label,
                    ai_watch_ble_get_status_text(ai_watch_ble_get_state()));
  lv_obj_set_style_text_color(g_ai_app.status_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(g_ai_app.status_label,
                             &lv_font_montserrat_14, 0);
  lv_obj_align(g_ai_app.status_label, LV_ALIGN_TOP_MID, 0, 58);

  g_ai_app.timer_label = lv_label_create(screen);
  lv_label_set_text(g_ai_app.timer_label, "");
  lv_obj_set_style_text_color(g_ai_app.timer_label,
                              ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_font(g_ai_app.timer_label,
                             &lv_font_montserrat_16, 0);
  lv_obj_align(g_ai_app.timer_label, LV_ALIGN_TOP_MID, 0, 82);

  g_ai_app.result_label = lv_label_create(screen);
  lv_label_set_text(g_ai_app.result_label,
                    "No AI result yet.\nPress Start, then speak\n"
                    "on the phone.");
  lv_obj_set_style_text_color(g_ai_app.result_label,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(g_ai_app.result_label,
                             &ai_watch_font_cjk_16, 0);
  lv_label_set_long_mode(g_ai_app.result_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_ai_app.result_label, 350);
  lv_obj_align(g_ai_app.result_label, LV_ALIGN_TOP_MID, 0, 112);

  start_btn = lv_btn_create(screen);
  lv_obj_set_size(start_btn, 170, 50);
  lv_obj_set_style_bg_color(start_btn, ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_radius(start_btn, 14, 0);
  lv_obj_align(start_btn, LV_ALIGN_BOTTOM_MID, 0, -80);
  {
    FAR lv_obj_t *mic_img = lv_img_create(start_btn);

    lv_img_set_src(mic_img, &icon_mic_t4);
    lv_obj_align(mic_img, LV_ALIGN_LEFT_MID, 14, 0);
  }

  start_lbl = lv_label_create(start_btn);
  lv_label_set_text(start_lbl, "Start");
  lv_obj_set_style_text_color(start_lbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(start_lbl, &lv_font_montserrat_16, 0);
  lv_obj_align(start_lbl, LV_ALIGN_RIGHT_MID, -18, 0);
  lv_obj_add_event_cb(start_btn, ai_watch_ai_start_cb,
                      LV_EVENT_CLICKED, watch);

  back_btn = lv_btn_create(screen);
  lv_obj_set_size(back_btn, 120, 45);
  lv_obj_set_style_bg_color(back_btn, ai_watch_theme_btn_bg(theme), 0);
  lv_obj_set_style_radius(back_btn, 12, 0);
  lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
  back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_color(back_lbl,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_16, 0);
  lv_obj_center(back_lbl);
  lv_obj_add_event_cb(back_btn, ai_watch_ai_back_cb,
                      LV_EVENT_CLICKED, watch);

  g_ai_app.waiting = false;
  g_ai_app.update_timer = lv_timer_create(
      ai_watch_ai_update_cb, AI_WATCH_AI_STATUS_TIMER_MS, watch);

  watch->app_page_screen = screen;
}

static void ai_watch_destroy_ai_app(FAR struct ai_watch_s *watch)
{
  if (g_ai_app.update_timer != NULL)
    {
      lv_timer_del(g_ai_app.update_timer);
      g_ai_app.update_timer = NULL;
    }

  /* NOTE: the countdown keeps running with the page closed */

  g_ai_app.status_label = NULL;
  g_ai_app.result_label = NULL;
  g_ai_app.timer_label = NULL;
  g_ai_app.waiting = false;

  /* NOTE: Do NOT delete app_page_screen here (see the timer app) */

  watch->app_page_screen = NULL;
}

static void ai_watch_ai_countdown_poll(FAR struct ai_watch_s *watch)
{
  struct ai_watch_ai_timer_req_s req;
  struct timespec now;

  /* Start any newly requested countdown (a newer request replaces a
   * running one).
   */

  while (ai_watch_ble_take_ai_timer(&req))
    {
      clock_gettime(CLOCK_MONOTONIC, &g_ai_app.cd_deadline);
      g_ai_app.cd_deadline.tv_sec += req.duration_s;
      g_ai_app.cd_running = true;
      g_ai_app.cd_total_s = req.duration_s;
      strlcpy(g_ai_app.cd_label, req.label, sizeof(g_ai_app.cd_label));

      printf("AI: countdown %lu s \"%s\"\n",
             (unsigned long)req.duration_s, req.label);
    }

  /* Fire the expiry banner */

  if (g_ai_app.cd_running)
    {
      struct ai_watch_ble_alert_s alert;

      clock_gettime(CLOCK_MONOTONIC, &now);

      if (now.tv_sec >= g_ai_app.cd_deadline.tv_sec)
        {
          g_ai_app.cd_running = false;

          memset(&alert, 0, sizeof(alert));
          alert.type = AI_WATCH_BLE_CMD_AI_TIMER;
          strlcpy(alert.title, g_ai_app.cd_label, sizeof(alert.title));
          ai_watch_alert_show(watch, &alert);
        }
    }
}

/****************************************************************************
 * Private Functions - About/Init
 ****************************************************************************/

static void ai_watch_init_about(FAR struct ai_watch_s *watch)
{
  char about[128];
  struct timespec ts;
  struct tm tm;
  bool rtc_ok = false;

  if (clock_gettime(CLOCK_REALTIME, &ts) == 0 &&
      ai_watch_ble_localtime(ts.tv_sec, &tm) != NULL &&
      tm.tm_year + 1900 >= AI_WATCH_RTC_MIN_YEAR &&
      tm.tm_year + 1900 <= AI_WATCH_RTC_MAX_YEAR)
    {
      char time[16];

      strftime(time, sizeof(time), "%H:%M:%S", &tm);
      snprintf(about, sizeof(about),
               "AI Watch v" AI_WATCH_VERSION "\n"
               "Board: SF32LB52\n"
               "Touch: %s\n"
               "RTC: %s",
               watch->touch_available ? "Active" : "Unavailable",
               time);
      rtc_ok = true;
    }
  else
    {
      snprintf(about, sizeof(about),
               "AI Watch v" AI_WATCH_VERSION "\n"
               "Board: SF32LB52\n"
               "Touch: %s\n"
               "RTC: Not set",
               watch->touch_available ? "Active" : "Unavailable");
    }

  lv_label_set_text(watch->about_label, about);

  if (rtc_ok)
    {
      watch->rtc_valid = true;
      watch->displayed_second = ts.tv_sec;
    }
  else
    {
      watch->rtc_valid = false;
      watch->rtc_warning_printed = true;
      printf("RTC time is not set; waiting for time synchronization\n");
    }
}

/* UI performance counters (2026-11-01 screen-fluidity work): refresh
 * cycles and invalidated pixel volume per second. The stock sysmon FPS
 * metric reads 0 on this port; these counters are the usable signal.
 * Expected magnitudes: idle watchface should be a couple of refreshes/s
 * with a few thousand dirty pixels; a value pinned at 15 refreshes of a
 * full 390x450 frame means something invalidates the whole screen. */

static uint32_t g_ui_refr_cnt;
static uint32_t g_ui_dirty_px;

static void ui_perf_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_REFR_START)
    {
      g_ui_refr_cnt++;
    }
  else if (code == LV_EVENT_INVALIDATE_AREA)
    {
      FAR lv_area_t *area = lv_event_get_param(e);

      if (area != NULL)
        {
          g_ui_dirty_px += (uint32_t)lv_area_get_width(area) *
                           lv_area_get_height(area);
        }
      else
        {
          g_ui_dirty_px +=
            (uint32_t)AI_WATCH_SCREEN_WIDTH * AI_WATCH_SCREEN_HEIGHT;
        }
    }
}

static void ui_perf_report_cb(lv_timer_t *timer)
{
  LV_UNUSED(timer);
  printf("UIPERF refr=%lu dirty=%lu\n",
         (unsigned long)g_ui_refr_cnt, (unsigned long)g_ui_dirty_px);
  g_ui_refr_cnt = 0;
  g_ui_dirty_px = 0;
}

static void ai_watch_create_ui(FAR struct ai_watch_s *watch)
{
  ai_watch_page_manager_init(watch);

  ai_watch_create_home_page(watch);
  ai_watch_create_app_list_page(watch);
  ai_watch_create_settings_page(watch);
  ai_watch_create_brief_page(watch);

  /* Input scan at 15 ms (default 30 ms reads as touch lag); report UI
   * load once a second. The refresh timer stays at the defconfig's
   * 16 ms. */

  {
    FAR lv_display_t *disp = lv_display_get_next(NULL);
    FAR lv_indev_t *indev = lv_indev_get_next(NULL);
    FAR lv_timer_t *timer;

    if (disp != NULL)
      {
        lv_display_add_event_cb(disp, ui_perf_event_cb, LV_EVENT_ALL, NULL);
      }

    while (indev != NULL)
      {
        timer = lv_indev_get_read_timer(indev);

        if (timer != NULL)
          {
            lv_timer_set_period(timer, 15);
          }

        indev = lv_indev_get_next(indev);
      }

    lv_timer_create(ui_perf_report_cb, 1000, NULL);
  }


  lv_scr_load_anim(watch->fixed_pages[AI_WATCH_PAGE_HOME],
                   LV_SCR_LOAD_ANIM_NONE, 0, 0, false);

  ai_watch_init_about(watch);
  ai_watch_time_update(watch);

  printf("UI created with %d fixed pages, %d registered apps\n",
         AI_WATCH_PAGE_FIXED_COUNT, (int)AI_WATCH_APP_COUNT);
}

/****************************************************************************
 * Private Functions - Button Input
 ****************************************************************************/

static int ai_watch_button_initialize(FAR struct ai_watch_s *watch)
{
  btn_buttonset_t sample;
  ssize_t nread;
  int ret;

  watch->button_fd = open(AI_WATCH_BUTTON_DEVICE, O_RDONLY | O_NONBLOCK);
  if (watch->button_fd < 0)
    {
      int errcode = errno;

      printf("ERROR: Failed to open %s: %d\n",
             AI_WATCH_BUTTON_DEVICE, errcode);
      return -errcode;
    }

  ret = ioctl(watch->button_fd, BTNIOC_SUPPORTED,
              (unsigned long)((uintptr_t)&watch->supported_buttons));
  if (ret < 0)
    {
      int errcode = errno;

      printf("ERROR: BTNIOC_SUPPORTED failed: %d\n", errcode);
      close(watch->button_fd);
      watch->button_fd = -1;
      return -errcode;
    }

  nread = read(watch->button_fd, &sample, sizeof(sample));
  if (nread != sizeof(sample))
    {
      int errcode = nread < 0 ? errno : EIO;

      printf("ERROR: Initial button read failed: %d\n", errcode);
      close(watch->button_fd);
      watch->button_fd = -1;
      return -errcode;
    }

  clock_gettime(CLOCK_MONOTONIC, &watch->next_button_poll);
  watch->button_raw_since = watch->next_button_poll;
  watch->button_raw_pressed = (sample & AI_WATCH_BUTTON_KEY2) != 0;
  watch->button_armed = !watch->button_raw_pressed;

  printf("Supported buttons: 0x%02lx\n",
         (unsigned long)watch->supported_buttons);
  printf("KEY2 button monitor started\n");
  return OK;
}

static void ai_watch_button_update(FAR struct ai_watch_s *watch,
                                   FAR const struct timespec *now)
{
  btn_buttonset_t sample;
  bool pressed;
  ssize_t nread;

  if (watch->button_fd < 0 ||
      !ai_watch_deadline_reached(now, &watch->next_button_poll))
    {
      return;
    }

  watch->next_button_poll = *now;
  ai_watch_advance_deadline(&watch->next_button_poll,
                            AI_WATCH_BUTTON_POLL_MS);

  nread = read(watch->button_fd, &sample, sizeof(sample));
  if (nread != sizeof(sample))
    {
      if (nread < 0 && errno != EAGAIN && errno != EINTR)
        {
          printf("ERROR: Button read failed: %d\n", errno);
        }

      return;
    }

  pressed = (sample & AI_WATCH_BUTTON_KEY2) != 0;
  if (pressed != watch->button_raw_pressed)
    {
      watch->button_raw_pressed = pressed;
      watch->button_raw_since = *now;
    }

  if (pressed)
    {
      if (watch->button_armed)
        {
          watch->button_armed = false;

          if (g_fall_ui.visible)
            {
              /* KEY2 doubles as the fall-alert cancel button */

              ai_watch_motion_fall_cancel();
            }
          else if (watch->current_page == AI_WATCH_PAGE_HOME)
            {
              /* On home page, KEY2 shows BLE status briefly.
               * BLE is managed automatically — no manual toggle.
               */

              printf("KEY2 pressed: BLE state=%d\n",
                     (int)ai_watch_ble_get_state());
            }
          else
            {
              ai_watch_pop_page(watch);
              printf("KEY2 pressed: Back\n");
            }
        }
    }
  else if (ai_watch_elapsed_ms(now, &watch->button_raw_since) >=
           AI_WATCH_BUTTON_RELEASE_MS)
    {
      watch->button_armed = true;
    }
}

/****************************************************************************
 * Private Functions - Time Update
 ****************************************************************************/

static void ai_watch_time_update(FAR struct ai_watch_s *watch)
{
  char date[32];
  char time_hm[8];
  char time_sec[8];
  char about[128];
  struct timespec ts;
  struct tm tm;
  static const char *day_names[] =
  {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
  };

  if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
    {
      return;
    }

  /* Hot path: the main loop runs ~100Hz - only convert/calendar work
   * when the displayed second actually changes. */

  if (ts.tv_sec == watch->displayed_second && watch->rtc_valid)
    {
      return;
    }

  if (ai_watch_ble_localtime(ts.tv_sec, &tm) == NULL ||
      tm.tm_year + 1900 < AI_WATCH_RTC_MIN_YEAR ||
      tm.tm_year + 1900 > AI_WATCH_RTC_MAX_YEAR)
    {
      if (!watch->rtc_warning_printed)
        {
          printf("RTC time is not set; waiting for time synchronization\n");
          watch->rtc_warning_printed = true;
        }

      if (watch->rtc_valid)
        {
          lv_label_set_text(watch->time_label, "--:--");
          lv_label_set_text(watch->seconds_label, "--");
          lv_label_set_text(watch->date_label, "----/--/--");
          watch->rtc_valid = false;

          snprintf(about, sizeof(about),
                   "AI Watch v" AI_WATCH_VERSION "\n"
                   "Board: SF32LB52\n"
                   "Touch: %s\n"
                   "RTC: Not set",
                   watch->touch_available ? "Active" : "Unavailable");
          lv_label_set_text(watch->about_label, about);
        }

      return;
    }

  watch->rtc_valid = true;
  if (ts.tv_sec == watch->displayed_second)
    {
      return;
    }

  watch->displayed_second = ts.tv_sec;

  strftime(time_hm, sizeof(time_hm), "%H:%M", &tm);
  snprintf(time_sec, sizeof(time_sec), "%02d", tm.tm_sec);

  /* Weekday and date share a single label (no overlap possible) */

  snprintf(date, sizeof(date), "%s  %04d/%02d/%02d",
           day_names[ai_watch_day_of_week(tm.tm_year + 1900,
                                          tm.tm_mon + 1, tm.tm_mday)],
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

  lv_label_set_text(watch->time_label, time_hm);
  lv_label_set_text(watch->seconds_label, time_sec);
  lv_label_set_text(watch->date_label, date);

  snprintf(about, sizeof(about),
           "AI Watch v" AI_WATCH_VERSION "\n"
           "Board: SF32LB52\n"
           "Touch: %s\n"
           "RTC: %s",
           watch->touch_available ? "Active" : "Unavailable",
           time_hm);
  lv_label_set_text(watch->about_label, about);
}

/****************************************************************************
 * Name: ai_watch_unread_update
 *
 * Description:
 *   Refresh the home-page unread indicator (bell + count). Reads the
 *   reminder store directly, so it does not depend on the reminder app
 *   being open or its pending flag.
 *
 ****************************************************************************/

static void ai_watch_unread_update(FAR struct ai_watch_s *watch)
{
  FAR struct ai_watch_reminder_store_s *store;
  static int last_unread = -1;
  char buf[32];
  int unread = 0;
  int i;

  store = ai_watch_ble_get_reminders();
  if (store != NULL)
    {
      for (i = 0; i < AI_WATCH_REMINDER_MAX; i++)
        {
          FAR const struct ai_watch_reminder_s *item = &store->items[i];

          if (item->id != 0 &&
              (item->flags & AI_WATCH_REMINDER_FLAG_READ) == 0)
            {
              unread++;
            }
        }
    }

  if (unread == last_unread)
    {
      return;
    }

  last_unread = unread;

  if (watch->unread_label == NULL)
    {
      return;
    }

  if (unread > 0)
    {
      snprintf(buf, sizeof(buf), LV_SYMBOL_BELL " %d unread", unread);
      lv_label_set_text(watch->unread_label, buf);
      lv_obj_clear_flag(watch->unread_label, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_add_flag(watch->unread_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/****************************************************************************
 * Private Functions - Touch Input
 ****************************************************************************/

static void ai_watch_touch_update(FAR struct ai_watch_s *watch)
{
  lv_indev_state_t state;
  lv_point_t point;
  struct timespec now;

  if (watch->touch_indev == NULL || !watch->touch_available)
    {
      return;
    }

  state = lv_indev_get_state(watch->touch_indev);
  lv_indev_get_point(watch->touch_indev, &point);
  clock_gettime(CLOCK_MONOTONIC, &now);

  if (state == LV_INDEV_STATE_PRESSED)
    {
      if (!watch->touch_active)
        {
          watch->touch_active = true;
          watch->gesture_handled = false;
          watch->gesture_lock = false;
          watch->touch_start_x = point.x;
          watch->touch_start_y = point.y;
          watch->touch_last_y = point.y;
          watch->touch_start_time = now;
        }
      else if (!watch->gesture_handled &&
               !watch->gesture_lock &&
               watch->current_page != AI_WATCH_PAGE_HOME &&
               watch->current_page != AI_WATCH_PAGE_APP_LIST)
        {
          /* App and settings pages: return home as soon as the drag
           * shows unambiguous horizontal intent - no release timing
           * needed. The hex app-list page is excluded so its own drag
           * handling is not affected, and gesture_lock keeps a drag on
           * the brightness bar from being read as a back swipe.
           */

            int dx = point.x - watch->touch_start_x;
            int dy = point.y - watch->touch_start_y;

            if (dx > AI_WATCH_SWIPE_BACK_TRIGGER &&
                abs(dx) > abs(dy) * 2)
              {
                watch->gesture_handled = true;
                ai_watch_go_home(watch);
              }
            else if (watch->current_page == AI_WATCH_PAGE_BRIEF &&
                     g_brief_ui.screen != NULL && abs(dy) > abs(dx))
              {
                /* The brief page scrolls with the finger. LVGL's own
                 * drag scroll never engages through the remote pointer
                 * indev -- press/move samples all arrive (verbose [PTR]
                 * trace 2026-11-01) yet the screen never moves -- so
                 * the poller drives the scroll itself. Finger up
                 * reveals lower cards; clamped to the content. */

                /* lv_obj_get_scroll_y() reads NEGATIVE of the
                 * scrolled-down amount on this LVGL (scroll 19 px down
                 * -> -19; feeding it back raw doubled the offset every
                 * poll and fled the content). Negate it. */

                int step = watch->touch_last_y - point.y;
                int cur = -(int)lv_obj_get_scroll_y(g_brief_ui.screen);
                int target = cur + step;

                watch->touch_last_y = point.y;

                if (target < 0)
                  {
                    target = 0;
                  }

                if (target > AI_WATCH_BRIEF_SCROLL_MAX)
                  {
                    target = AI_WATCH_BRIEF_SCROLL_MAX;
                  }

                if (target != cur)
                  {
                    lv_obj_scroll_by(g_brief_ui.screen, 0,
                                     (lv_coord_t)(target - cur),
                                     LV_ANIM_OFF);
                  }
              }
        }
    }
  else if (watch->touch_active)
    {
      int dx = point.x - watch->touch_start_x;
      int dy = point.y - watch->touch_start_y;
      int64_t elapsed_ms =
          ai_watch_elapsed_ms(&now, &watch->touch_start_time);
      bool locked = watch->gesture_lock;

      watch->touch_active = false;
      watch->gesture_lock = false;

      /* The release has to be suppressed too: LVGL delivers it before this
       * poll runs, so a horizontal drag on the bar would otherwise be scored
       * as a deliberate back swipe by the fallback below.
       */

      if (watch->gesture_handled || locked)
        {
          return;
        }

      if (watch->current_page == AI_WATCH_PAGE_HOME)
        {
          /* Home page: swipe up opens the app list, horizontal swipes
           * open the brief. NO time gate here: the classification is
           * purely geometric (|dx|/|dy| vs the threshold), and the
           * remote pointer's drag duration is unbounded - the old 300/
           * 600 ms windows silently swallowed human-paced remote swipes
           * (measured 800 ms). A long stationary press ends in the
           * small-delta case, which does nothing since taps no longer
           * open the list. */

          if (abs(dy) > AI_WATCH_SWIPE_THRESHOLD && abs(dy) > abs(dx))
            {
              if (dy < 0)
                {
                  ai_watch_push_page(watch, AI_WATCH_PAGE_APP_LIST);
                }
            }
          else if (abs(dx) > AI_WATCH_SWIPE_THRESHOLD && abs(dx) > abs(dy))
            {
              /* Either horizontal direction opens the brief. Left swipe
               * is the documented gesture; right swipe was added for the
               * remote-preview era - users naturally swipe right for a
               * -1 screen, and with only the left branch a right swipe
               * fell through to the widget underneath and read as a
               * click. Refresh first so the page shows counters as of
               * now rather than as of creation.
               */

              ai_watch_brief_refresh(watch);
              ai_watch_push_page(watch, AI_WATCH_PAGE_BRIEF);
            }
          /* Taps no longer open the app list (removed 2026-11-01): over
           * the remote preview a swipe that misclassifies as a tap kept
           * opening the wrong page - the list is swipe-up only now. */

          (void)dx;
          (void)dy;
        }
      else if (watch->current_page != AI_WATCH_PAGE_APP_LIST)
        {
          /* Slow deliberate swipes: release-based fallback */

          if (elapsed_ms > AI_WATCH_SWIPE_BACK_TIMEOUT_MS)
            {
              return;
            }

          if (dx > AI_WATCH_SWIPE_THRESHOLD && abs(dx) > abs(dy))
            {
              ai_watch_go_home(watch);
            }
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* LCD partial-render plumbing (2026-11-01 screen-fluidity work): LVGL
 * renders invalidated strips into a small SRAM buffer; this flush sends
 * each strip to the panel and mirrors it into a full-frame shadow so the
 * shot link can still snapshot complete pictures (in PARTIAL mode the
 * LVGL buffer only ever holds one strip). */

static int g_lcd_fd = -1;
static FAR uint16_t *g_lcd_shadow;

/* Render strip in .bss: guaranteed SRAM (the heap's 351 KB full-frame
 * buffer lands in PSRAM and LVGL renders through the QSPI bus at
 * ~4 MB/s -- the dominant screen-fluidity bottleneck on this board). */

static FAR uint16_t g_lcd_strip[AI_WATCH_SCREEN_WIDTH * AI_WATCH_LCD_STRIP_ROWS]
  __attribute__((aligned(64)));

static void ai_watch_lcd_flush_cb(lv_display_t *disp, const lv_area_t *area,
                                  uint8_t *color_p)
{
  struct lcddev_area_s desc;
  int y;
  int row_bytes;

  desc.row_start = area->y1;
  desc.row_end   = area->y2;
  desc.col_start = area->x1;
  desc.col_end   = area->x2;
  desc.data      = color_p;
  row_bytes      = (area->x2 - area->x1 + 1) * 2;   /* RGB565 */
  desc.stride    = row_bytes;

  if (g_lcd_fd >= 0)
    {
      ioctl(g_lcd_fd, LCDDEVIO_PUTAREA, (unsigned long)&desc);
    }

  if (g_lcd_shadow != NULL)
    {
      for (y = area->y1; y <= area->y2; y++)
        {
          memcpy(g_lcd_shadow + y * AI_WATCH_SCREEN_WIDTH + area->x1,
                 color_p + (y - area->y1) * row_bytes, row_bytes);
        }
    }

  lv_display_flush_ready(disp);
}

int main(int argc, FAR char *argv[])
{
  lv_nuttx_result_t result;
  lv_nuttx_dsc_t info;
  struct timespec radio_ready_since = {0, 0};
  bool sensor_features_started = false;
  struct ai_watch_s watch =
  {
    .button_fd = -1,
    .displayed_second = -1,
    .current_page = AI_WATCH_PAGE_HOME,
    .current_theme = AI_WATCH_THEME_DARK,
    .touch_indev = NULL,
    .touch_available = false,
    .page_stack_top = 0,
    .app_page_screen = NULL,
    .active_app_index = -1,
    .settings_bt_enabled = false,
    .bt_switch = NULL,
    .ble_state_label = NULL,
    .timer =
    {
      .update_timer = NULL,
      .start_tick = 0,
      .elapsed_ms = 0,
      .running = false,
      .time_label = NULL,
    },
    .menu =
    {
      .container = NULL,
      .icon_count = 0,
      .scroll_x = 0.0f,
      .scroll_y = 0.0f,
      .dragging = false,
      .focused_index = 0,
      .snapping = false,
    },
  };

  /* The version string alone cannot tell two builds apart -- 4.8.35 shipped
   * several times with different contents, which made "which one is on the
   * board?" a recurring question. The build stamp answers it directly.
   */

  printf("AI Watch version %s\n", AI_WATCH_VERSION);
  printf("AI Watch build %s %s\n", __DATE__, __TIME__);
  printf("ai_watch started\n");

  {
    /* Heap diagnostics: arena must end below the LCPU mailbox window
     * (0x2007FC00) or the BT controller stream gets corrupted.
     */

    struct mallinfo mi = mallinfo();

    printf("heap arena %u bytes\n", (unsigned int)mi.arena);
  }

  /* Wait for LCD device (mandatory).  Touch will be added later. */

  int retry;

  for (retry = 0; retry < 100; retry++)
    {
      if (access("/dev/lcd0", F_OK) == 0)
        {
          printf("LCD device ready after %d ms\n", retry * 100);
          break;
        }

      usleep(100000);
    }

  if (retry >= 100)
    {
      printf("ERROR: /dev/lcd0 not found after 10s\n");
      return EXIT_FAILURE;
    }

  if (lv_is_initialized())
    {
      printf("ERROR: LVGL is already initialized\n");
      return EXIT_FAILURE;
    }

  lv_init();

  int init_retry = 0;

  LV_UNUSED(info);

#if AI_WATCH_HEADLESS
  /* Render-only display: a full-frame RGB565 buffer plus a flush callback
   * that immediately reports the transfer as done. Nothing is written to
   * the LCD, so no putarea() timeout can stall the UI.
   */

  lv_tick_set_cb(ai_watch_headless_tick);

  result.disp = lv_display_create(390, 450);

  if (result.disp != NULL)
    {
      /* PARTIAL strip in SRAM + full-frame shadow (PSRAM): DIRECT with a
       * full-frame buffer redrew 351 KB through PSRAM at ~4 MB/s
       * (scroll render 88 ms); the SRAM strip renders an order faster
       * and the flush mirrors each strip into the shadow that the shot
       * link snapshots. PARTIAL renders only the damaged areas. */

      g_lcd_shadow = lv_malloc((size_t)AI_WATCH_SCREEN_WIDTH *
                               AI_WATCH_SCREEN_HEIGHT * 2);

      if (g_lcd_shadow != NULL)
        {
          lv_display_set_color_format(result.disp, LV_COLOR_FORMAT_RGB565);
          lv_display_set_buffers(result.disp, g_lcd_strip, NULL,
                                 sizeof(g_lcd_strip),
                                 LV_DISPLAY_RENDER_MODE_PARTIAL);
          lv_display_set_flush_cb(result.disp, ai_watch_lcd_flush_cb);
          ai_watch_shot_bind_frame((FAR const uint8_t *)g_lcd_shadow,
                                   AI_WATCH_SCREEN_WIDTH,
                                   AI_WATCH_SCREEN_HEIGHT);
          printf("Headless display: SRAM strip + shadow frame\n");
        }
      else
        {
          printf("ERROR: headless shadow frame alloc failed\n");
          result.disp = NULL;
        }
    }

#else
  lv_nuttx_dsc_init(&info);

  info.fb_path = "/dev/lcd0";
  info.input_path = NULL;  /* skip touch — added later when ready */

  /* Retry lv_nuttx_init — the driver may still be completing
   * the first DMA transfer when /dev/lcd0 first becomes openable.
   */

  for (init_retry = 0; ; init_retry++)
    {
      lv_nuttx_init(&info, &result);

      if (result.disp != NULL)
        {
          break;
        }

      printf("lv_nuttx_init attempt %d failed, retrying...\n",
             init_retry + 1);
      lv_nuttx_deinit(&result);
      lv_deinit();
      usleep(200000); /* 200 ms */
      lv_init();
      lv_nuttx_dsc_init(&info);
      info.fb_path = "/dev/lcd0";
      info.input_path = NULL;
    }
#endif /* AI_WATCH_HEADLESS */

  printf("LVGL initialized, display ready after %d attempt(s)\n",
         init_retry + 1);

#if !AI_WATCH_HEADLESS
  /* Screen fluidity (2026-11-01): lv_nuttx_lcd hands LVGL a full-screen
   * buffer in RENDER_MODE_FULL -- every invalidation re-rendered every
   * widget AND re-streamed the whole 351 KB frame over QSPI (measured
   * 100-180 ms per invalidation; scroll render averaged 88 ms even in
   * DIRECT, because the full-frame buffer lands in PSRAM and LVGL
   * renders at ~4 MB/s through the QSPI bus). Switch to a 100-row strip
   * in SRAM (LV_DISPLAY_RENDER_MODE_PARTIAL): only the invalidated
   * areas are rendered and streamed. The strip cannot feed the shot
   * mirror, so the flush mirrors every strip into a full-frame shadow
   * (PSRAM) that the snapshot path reads instead of the LVGL buffer.
   * The port's original full-screen buffer stays allocated (no free
   * API); one-off 351 KB in an 8 MB PSRAM. */

  if (result.disp != NULL)
    {
      FAR lv_color_t *strip =
        lv_malloc((size_t)AI_WATCH_SCREEN_WIDTH *
                  AI_WATCH_LCD_STRIP_ROWS * 2);

      g_lcd_shadow = lv_malloc((size_t)AI_WATCH_SCREEN_WIDTH *
                               AI_WATCH_SCREEN_HEIGHT * 2);
      g_lcd_fd = open("/dev/lcd0", O_RDWR);

      if (strip != NULL && g_lcd_shadow != NULL && g_lcd_fd >= 0)
        {
          lv_display_set_buffers(result.disp, strip, NULL,
                                 (size_t)AI_WATCH_SCREEN_WIDTH *
                                 AI_WATCH_LCD_STRIP_ROWS * 2,
                                 LV_DISPLAY_RENDER_MODE_PARTIAL);
          lv_display_set_flush_cb(result.disp, ai_watch_lcd_flush_cb);
          ai_watch_shot_bind_frame((FAR const uint8_t *)g_lcd_shadow,
                                   AI_WATCH_SCREEN_WIDTH,
                                   AI_WATCH_SCREEN_HEIGHT);
          printf("LCD partial render: %d-row strip + shadow frame\n",
                 AI_WATCH_LCD_STRIP_ROWS);
        }
      else
        {
          /* Keep the port's full-screen buffer (DIRECT); slower, but the
           * UI and the mirror keep working. */
          lv_display_set_render_mode(result.disp,
                                     LV_DISPLAY_RENDER_MODE_DIRECT);
          printf("LCD partial setup failed; DIRECT full-frame fallback\n");
        }
    }
#endif /* !AI_WATCH_HEADLESS */

  /* Screen dump hook. The display buffer holds a full frame in this
   * configuration, so a dump is a copy of what LVGL already rendered.
   */

  ai_watch_shot_init(result.disp);

  /* Touch may not be registered yet — try now, poll later if not */

  watch.touch_indev = NULL;
  watch.touch_available = false;
  watch.touch_poll_remaining = 30;  /* poll for up to 30s */

#if !AI_WATCH_HEADLESS
  /* The panel's own touch controller. Skipped entirely in headless mode:
   * /dev/input0 can still exist there (the driver registers it even when
   * the hardware is gone), and binding that dead device would shadow the
   * host pointer below.
   */

  if (access("/dev/input0", F_OK) == 0)
    {
      watch.touch_indev =
          lv_nuttx_touchscreen_create("/dev/input0");

      if (watch.touch_indev != NULL)
        {
          watch.touch_available = true;
          watch.touch_poll_remaining = 0;
          printf("Touch input initialized\n");
        }
    }
#else
  printf("Headless mode: panel touch skipped\n");
#endif

  if (!watch.touch_available)
    {
      /* No panel touch layer. Fall back to the host pointer registered by
       * the screen-dump hook: without this, every gesture handler bails
       * out early and clicks from the preview window do nothing.
       */

      watch.touch_indev = ai_watch_shot_get_indev();

      if (watch.touch_indev != NULL)
        {
          watch.touch_available = true;
          watch.touch_poll_remaining = 0;
          printf("Touch unavailable; using remote pointer from host\n");
        }
      else
        {
          printf("Touch not ready, will poll every 1s\n");
          clock_gettime(CLOCK_MONOTONIC, &watch.touch_next_poll);
          watch.touch_next_poll.tv_sec += 1;
        }
    }

  ai_watch_create_ui(&watch);
  ai_watch_button_initialize(&watch);

  /* Start BLE bring-up (asynchronous; state is polled in the main loop) */

  /* Background motion service (onboard IMU): steps, activity and
   * suspected-fall detection run from boot, independent of the UI.
   */

  ai_watch_motion_init();

  /* Device-side skills: sit reminder, step-goal cheer and the daily
   * brief slot. They read the motion snapshot and post events to the
   * loop below; nothing here talks to BLE by itself.
   */

  ai_watch_skill_init(0);

  ai_watch_ble_init();
  ai_watch_update_bt_label(&watch);

  printf("UI created; entering main loop\n");

  for (; ; )
    {
      struct timespec now;
      uint32_t idle;

      clock_gettime(CLOCK_MONOTONIC, &now);

      /* Poll for touch device (every 1s, up to 30 attempts) */

      if (!watch.touch_available && watch.touch_poll_remaining > 0)
        {
          if (ai_watch_deadline_reached(&now,
                                        &watch.touch_next_poll))
            {
              if (access("/dev/input0", F_OK) == 0)
                {
                  watch.touch_indev =
                      lv_nuttx_touchscreen_create("/dev/input0");

                  if (watch.touch_indev != NULL)
                    {
                      watch.touch_available = true;
                      watch.touch_poll_remaining = 0;
                      printf("Touch input initialized (delayed)\n");
                    }
                }

              watch.touch_poll_remaining--;
              watch.touch_next_poll = now;
              watch.touch_next_poll.tv_sec += 1;
            }
        }

      ai_watch_button_update(&watch, &now);
      ai_watch_touch_update(&watch);
      ai_watch_time_update(&watch);

      /* Process pending BLE events (time sync, command frames) and
       * refresh the status label on state transitions.
       */

      {
        static enum ai_watch_ble_bsp_state_e last_ble_state =
          AI_WATCH_BLE_BSP_OFF;

        enum ai_watch_ble_bsp_state_e state;

        ai_watch_ble_process();
        state = ai_watch_ble_get_state();

        if (state != last_ble_state)
          {
            last_ble_state = state;
            ai_watch_update_bt_label(&watch);
          }

        /* Sensor and panel setup can contend for HCPU peripherals while the
         * controller is recovering. Wait until BLE has stayed usable for
         * ten seconds before touching I2C, ADC or LCD power controls.
         */

        if (state == AI_WATCH_BLE_BSP_ADVERTISING ||
            state == AI_WATCH_BLE_BSP_CONNECTED)
          {
            if (radio_ready_since.tv_sec == 0)
              {
                clock_gettime(CLOCK_MONOTONIC, &radio_ready_since);
              }
            else if (!sensor_features_started &&
                     ai_watch_elapsed_ms(&now, &radio_ready_since) >= 10000)
              {
                int light_ret;

                light_ret = ai_watch_light_init();
                if (light_ret >= 0)
                  {
                    ai_watch_light_start();
                  }

                ai_watch_battery_init();
                sensor_features_started = true;
                printf("Sensors: deferred startup complete (light=%d)\n",
                       light_ret);
              }
          }
        else
          {
            radio_ready_since.tv_sec = 0;
            radio_ready_since.tv_nsec = 0;
          }

        /* Show an alert banner for every arrival not shown yet */

        {
          struct ai_watch_ble_alert_s alert;

          while (ai_watch_ble_take_alert(&alert))
            {
              ai_watch_alert_show(&watch, &alert);
            }
        }
      }

      /* AI countdown: start requested timers, fire expiry banners */

      ai_watch_ai_countdown_poll(&watch);

      /* Motion service: recording drain + BLE pacing + fall overlay */

      ai_watch_motion_process();
      ai_watch_fall_poll(&watch);

      /* Device skills: surface whatever the skill worker posted. These
       * arrive as ordinary alert banners, so they reuse the existing
       * show/hide lifecycle instead of adding a second overlay.
       */

      /* BLE connect window: keep the fragile post-connect period free
       * of preview until the phone syncs (8 s cap). The IMU stays at the
       * p02-verified 15 Hz for the WHOLE connection: the phone subscribes
       * in indication mode (CCCD 0x0002 -> bt_gatt_indicate, ~100-150 ms
       * blocking each), and the MOTION_DATA indication rate is
       * sample_rate/4 - at 25 ms poll that is ~10/s, 2.7x the p02-era
       * rate, and connects died right as that stream started. */

      {
        bool conn_window = ai_watch_ble_connect_window_active();

        ai_watch_shot_set_stream_gate(conn_window);
        ai_watch_motion_set_slow(ai_watch_ble_get_state() ==
                                 AI_WATCH_BLE_BSP_CONNECTED);
      }

      ai_watch_skill_poll_ui(&watch);

      ai_watch_unread_update(&watch);
      ai_watch_settings_poll(&watch);

      /* A brief that lands while the page is open updates in place;
       * otherwise it is picked up by the refresh on entry.
       */

      if (watch.current_page == AI_WATCH_PAGE_BRIEF &&
          ai_watch_ble_get_brief()->pending)
        {
          ai_watch_brief_refresh(&watch);
        }

      /* Panel writes happen here, on the same thread that runs the LVGL
       * flush, never from the sensor worker.
       */

      ai_watch_light_poll();
      ai_watch_battery_poll();
      ai_watch_battery_update(&watch);

      /* Draw the touch marker before the render pass so it lands in the
       * same frame the dump will pick up.
       */

      ai_watch_touch_probe_poll(&watch);

      idle = lv_timer_handler();

      /* Consume a console keystroke, if any. Placed after the LVGL pass so
       * a dump always captures a fully rendered frame.
       */

      ai_watch_shot_poll();

      if (idle == 0 || idle > AI_WATCH_BUTTON_POLL_MS)
        {
          idle = AI_WATCH_BUTTON_POLL_MS;
        }

      usleep(idle * 1000);
    }

  return EXIT_SUCCESS;
}
