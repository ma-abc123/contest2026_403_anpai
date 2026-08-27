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
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/input/buttons.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_touchscreen.h>

#include "ai_watch_icons.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AI_WATCH_VERSION            "4.0.0"
#define AI_WATCH_BUTTON_DEVICE      "/dev/buttons"
#define AI_WATCH_BUTTON_KEY2        (1 << 0)
#define AI_WATCH_BUTTON_POLL_MS     10
#define AI_WATCH_BUTTON_RELEASE_MS  80
#define AI_WATCH_RTC_MIN_YEAR       2020
#define AI_WATCH_INPUT_DEVICE       "/dev/input0"

/* Display dimensions */

#define AI_WATCH_SCREEN_WIDTH       390
#define AI_WATCH_SCREEN_HEIGHT      450

/* Fixed page IDs */

#define AI_WATCH_PAGE_HOME          0
#define AI_WATCH_PAGE_APP_LIST      1
#define AI_WATCH_PAGE_SETTINGS      2
#define AI_WATCH_PAGE_FIXED_COUNT   3

/* App base page ID (dynamic pages start here) */

#define AI_WATCH_PAGE_APP_BASE      100

/* Page stack */

#define AI_WATCH_PAGE_STACK_MAX     8

/* Swipe gesture thresholds */

#define AI_WATCH_SWIPE_THRESHOLD    50
#define AI_WATCH_SWIPE_TIMEOUT_MS   300

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
  FAR lv_obj_t *time_label;
  FAR lv_obj_t *seconds_label;
  FAR lv_obj_t *date_label;
  FAR lv_obj_t *dow_label;
  FAR lv_obj_t *bt_label;

  /* Display objects - App list page */

  FAR lv_obj_t *app_list_screen;
  struct ai_menu_ctx_s menu;

  /* Display objects - Settings page */

  FAR lv_obj_t *settings_screen;
  FAR lv_obj_t *bt_switch;
  FAR lv_obj_t *theme_roller;
  FAR lv_obj_t *about_label;

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
  bool touch_available;
  int touch_start_x;
  int touch_start_y;
  struct timespec touch_start_time;

  /* Touch polling */

  struct timespec touch_next_poll;
  int touch_poll_remaining;

  /* RTC state */

  time_t displayed_second;
  bool rtc_valid;
  bool rtc_warning_printed;

  /* Application state */

  bool bt_state;
  bool settings_bt_enabled;
  int current_theme;

  /* Timer state */

  struct ai_timer_state_s timer;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Page management */

static void ai_watch_push_page(FAR struct ai_watch_s *watch, int page);
static void ai_watch_pop_page(FAR struct ai_watch_s *watch);
static void ai_watch_open_app(FAR struct ai_watch_s *watch, int app_index);

/* Theme / state */

static void ai_watch_update_bt_state(FAR struct ai_watch_s *watch,
                                     bool new_state);
static void ai_watch_update_theme(FAR struct ai_watch_s *watch, int theme);

/* UI creation */

static void ai_watch_create_home_page(FAR struct ai_watch_s *watch);
static void ai_watch_create_app_list_page(FAR struct ai_watch_s *watch);
static void ai_watch_create_settings_page(FAR struct ai_watch_s *watch);

/* Timer app */

static void ai_watch_create_timer_app(FAR struct ai_watch_s *watch);
static void ai_watch_destroy_timer_app(FAR struct ai_watch_s *watch);
static void ai_watch_timer_update_cb(FAR lv_timer_t *timer);

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
    "Exercise",
    { &icon_exercise_t0, &icon_exercise_t1, &icon_exercise_t2,
      &icon_exercise_t3, &icon_exercise_t4 },
    NULL, NULL, false
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
    NULL, NULL, false
  },
  {
    "Settings",
    { &icon_settings_t0, &icon_settings_t1, &icon_settings_t2,
      &icon_settings_t3, &icon_settings_t4 },
    NULL, NULL, true
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
                  if (i == ctx->focused_index)
                    {
                      /* Tapped the focused icon: launch app */

                      ai_watch_open_app(watch,
                                        ctx->icons[i].app_index);
                    }
                  else
                    {
                      /* Tapped a non-focused icon: snap to it */

                      ai_menu_start_snap(watch, i);
                    }

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

static void ai_watch_update_theme(FAR struct ai_watch_s *watch, int theme)
{
  uint32_t home_child_count;
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
  lv_obj_set_style_text_color(
      lv_obj_get_child(watch->home_screen, 0),
      ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_color(watch->time_label,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_color(watch->seconds_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_color(watch->date_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_color(watch->dow_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_color(watch->bt_label,
                              ai_watch_theme_accent(theme), 0);

  home_child_count = lv_obj_get_child_count(watch->home_screen);
  if (home_child_count > 0)
    {
      lv_obj_set_style_text_color(
          lv_obj_get_child(watch->home_screen, home_child_count - 1),
          ai_watch_theme_secondary(theme), 0);
    }

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

      settings_child_count =
          lv_obj_get_child_count(watch->settings_screen);

      for (i = 0; i < settings_child_count; i++)
        {
          FAR lv_obj_t *child =
              lv_obj_get_child(watch->settings_screen, i);

          if (child == watch->about_label)
            {
              lv_obj_set_style_text_color(
                  child, ai_watch_theme_secondary(theme), 0);
            }
          else if (child != watch->bt_switch &&
                   child != watch->theme_roller)
            {
              lv_obj_set_style_text_color(
                  child, ai_watch_theme_text(theme), 0);
            }
        }
    }
}

/****************************************************************************
 * Private Functions - State Management
 ****************************************************************************/

static void ai_watch_update_bt_state(FAR struct ai_watch_s *watch,
                                     bool new_state)
{
  watch->bt_state = new_state;
  watch->settings_bt_enabled = new_state;

  lv_label_set_text(watch->bt_label,
                    watch->bt_state ? "BT: ON" : "BT: OFF");

  if (watch->bt_switch != NULL)
    {
      if (watch->bt_state)
        {
          lv_obj_add_state(watch->bt_switch, LV_STATE_CHECKED);
        }
      else
        {
          lv_obj_clear_state(watch->bt_switch, LV_STATE_CHECKED);
        }
    }

  printf("Bluetooth %s\n", watch->bt_state ? "enabled" : "disabled");
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
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

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
 * Private Functions - UI Callbacks
 ****************************************************************************/

static void ai_watch_settings_bt_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);
  FAR lv_obj_t *sw = lv_event_get_target(e);
  bool new_state = lv_obj_has_state(sw, LV_STATE_CHECKED);

  ai_watch_update_bt_state(watch, new_state);
}

static void ai_watch_settings_theme_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);
  FAR lv_obj_t *roller = lv_event_get_target(e);
  int theme = lv_roller_get_selected(roller);

  ai_watch_update_theme(watch, theme);
}

static void ai_watch_home_click_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);

  if (watch->current_page == AI_WATCH_PAGE_HOME)
    {
      ai_watch_push_page(watch, AI_WATCH_PAGE_APP_LIST);
    }
}

/****************************************************************************
 * Private Functions - Settings Page
 ****************************************************************************/

static void ai_watch_create_settings_page(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen = lv_obj_create(NULL);
  FAR lv_obj_t *title;
  FAR lv_obj_t *bt_label;
  FAR lv_obj_t *theme_label;
  FAR lv_obj_t *hint;
  int theme = watch->current_theme;

  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(theme), 0);
  watch->settings_screen = screen;
  watch->fixed_pages[AI_WATCH_PAGE_SETTINGS] = screen;

  title = lv_label_create(screen);
  lv_label_set_text(title, "Settings");
  lv_obj_set_style_text_color(title, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

  bt_label = lv_label_create(screen);
  lv_label_set_text(bt_label, "Bluetooth");
  lv_obj_set_style_text_color(bt_label, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(bt_label, &lv_font_montserrat_16, 0);
  lv_obj_align(bt_label, LV_ALIGN_TOP_LEFT, 30, 80);

  watch->bt_switch = lv_switch_create(screen);
  lv_obj_set_size(watch->bt_switch, 50, 25);
  lv_obj_align(watch->bt_switch, LV_ALIGN_TOP_RIGHT, -30, 75);
  if (watch->bt_state)
    {
      lv_obj_add_state(watch->bt_switch, LV_STATE_CHECKED);
    }

  lv_obj_add_event_cb(watch->bt_switch, ai_watch_settings_bt_cb,
                      LV_EVENT_VALUE_CHANGED, watch);

  theme_label = lv_label_create(screen);
  lv_label_set_text(theme_label, "Theme");
  lv_obj_set_style_text_color(theme_label, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(theme_label, &lv_font_montserrat_16, 0);
  lv_obj_align(theme_label, LV_ALIGN_TOP_LEFT, 30, 130);

  watch->theme_roller = lv_roller_create(screen);
  lv_roller_set_options(watch->theme_roller,
                       "Dark\n"
                       "Light\n"
                       "Blue",
                       LV_ROLLER_MODE_NORMAL);
  lv_obj_set_width(watch->theme_roller, 150);
  lv_obj_align(watch->theme_roller, LV_ALIGN_TOP_MID, 0, 160);
  lv_roller_set_visible_row_count(watch->theme_roller, 2);
  lv_roller_set_selected(watch->theme_roller, watch->current_theme,
                         LV_ANIM_OFF);

  lv_obj_add_event_cb(watch->theme_roller, ai_watch_settings_theme_cb,
                      LV_EVENT_VALUE_CHANGED, watch);

  watch->about_label = lv_label_create(screen);
  lv_label_set_text(watch->about_label,
                    "AI Watch v" AI_WATCH_VERSION "\n"
                    "Board: SF32LB52\n"
                    "Touch: Checking...\n"
                    "RTC: Checking...");
  lv_obj_set_style_text_color(watch->about_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(watch->about_label, &lv_font_montserrat_14, 0);
  lv_obj_align(watch->about_label, LV_ALIGN_CENTER, 0, 50);

  hint = lv_label_create(screen);
  lv_label_set_text(hint, LV_SYMBOL_LEFT " Back (KEY2)");
  lv_obj_set_style_text_color(hint, ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

/****************************************************************************
 * Private Functions - Home Page (Digital Watchface)
 ****************************************************************************/

static void ai_watch_create_home_page(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen = lv_obj_create(NULL);
  FAR lv_obj_t *title;
  FAR lv_obj_t *hint;
  int theme = watch->current_theme;

  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(theme), 0);
  watch->home_screen = screen;
  watch->fixed_pages[AI_WATCH_PAGE_HOME] = screen;

  title = lv_label_create(screen);
  lv_label_set_text(title, "AI Watch");
  lv_obj_set_style_text_color(title, ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

  watch->time_label = lv_label_create(screen);
  lv_label_set_text(watch->time_label, "--:--");
  lv_obj_set_style_text_color(watch->time_label,
                              ai_watch_theme_text(theme), 0);
  lv_obj_set_style_text_font(watch->time_label, &lv_font_montserrat_48, 0);
  lv_obj_align(watch->time_label, LV_ALIGN_CENTER, 0, -40);

  watch->seconds_label = lv_label_create(screen);
  lv_label_set_text(watch->seconds_label, ":--");
  lv_obj_set_style_text_color(watch->seconds_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(watch->seconds_label,
                              &lv_font_montserrat_24, 0);
  lv_obj_align_to(watch->seconds_label, watch->time_label,
                  LV_ALIGN_OUT_RIGHT_MID, 5, 8);

  watch->date_label = lv_label_create(screen);
  lv_label_set_text(watch->date_label, "----/--/--");
  lv_obj_set_style_text_color(watch->date_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(watch->date_label, &lv_font_montserrat_16, 0);
  lv_obj_align_to(watch->date_label, watch->time_label,
                  LV_ALIGN_OUT_BOTTOM_MID, -20, 12);

  watch->dow_label = lv_label_create(screen);
  lv_label_set_text(watch->dow_label, "");
  lv_obj_set_style_text_color(watch->dow_label,
                              ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(watch->dow_label, &lv_font_montserrat_16, 0);
  lv_obj_align_to(watch->dow_label, watch->date_label,
                  LV_ALIGN_OUT_RIGHT_MID, 10, 0);

  watch->bt_label = lv_label_create(screen);
  lv_label_set_text(watch->bt_label, "BT: OFF");
  lv_obj_set_style_text_color(watch->bt_label,
                              ai_watch_theme_accent(theme), 0);
  lv_obj_set_style_text_font(watch->bt_label, &lv_font_montserrat_20, 0);
  lv_obj_align(watch->bt_label, LV_ALIGN_BOTTOM_MID, 0, -60);

  hint = lv_label_create(screen);
  lv_label_set_text(hint, "Tap or swipe up for apps");
  lv_obj_set_style_text_color(hint, ai_watch_theme_secondary(theme), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

  lv_obj_add_event_cb(screen, ai_watch_home_click_cb,
                      LV_EVENT_CLICKED, watch);
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
 * Private Functions - About/Init
 ****************************************************************************/

static void ai_watch_init_about(FAR struct ai_watch_s *watch)
{
  char about[128];
  struct timespec ts;
  struct tm tm;
  bool rtc_ok = false;

  if (clock_gettime(CLOCK_REALTIME, &ts) == 0 &&
      localtime_r(&ts.tv_sec, &tm) != NULL &&
      tm.tm_year + 1900 >= AI_WATCH_RTC_MIN_YEAR)
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

static void ai_watch_create_ui(FAR struct ai_watch_s *watch)
{
  ai_watch_page_manager_init(watch);

  ai_watch_create_home_page(watch);
  ai_watch_create_app_list_page(watch);
  ai_watch_create_settings_page(watch);

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

          if (watch->current_page == AI_WATCH_PAGE_HOME)
            {
              ai_watch_update_bt_state(watch, !watch->bt_state);
              printf("KEY2 pressed: BT %s\n",
                     watch->bt_state ? "ON" : "OFF");
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
  char dow[8];
  char about[128];
  struct timespec ts;
  struct tm tm;
  static const char *day_names[] =
  {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
  };

  if (clock_gettime(CLOCK_REALTIME, &ts) < 0 ||
      localtime_r(&ts.tv_sec, &tm) == NULL ||
      tm.tm_year + 1900 < AI_WATCH_RTC_MIN_YEAR)
    {
      if (!watch->rtc_warning_printed)
        {
          printf("RTC time is not set; waiting for time synchronization\n");
          watch->rtc_warning_printed = true;
        }

      if (watch->rtc_valid)
        {
          lv_label_set_text(watch->time_label, "--:--");
          lv_label_set_text(watch->seconds_label, ":--");
          lv_label_set_text(watch->date_label, "----/--/--");
          lv_label_set_text(watch->dow_label, "");
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
  snprintf(time_sec, sizeof(time_sec), ":%02d", tm.tm_sec);
  strftime(date, sizeof(date), "%Y/%m/%d", &tm);

  int wday = ai_watch_day_of_week(tm.tm_year + 1900,
                                  tm.tm_mon + 1, tm.tm_mday);

  snprintf(dow, sizeof(dow), "%s", day_names[wday]);

  lv_label_set_text(watch->time_label, time_hm);
  lv_label_set_text(watch->seconds_label, time_sec);
  lv_label_set_text(watch->date_label, date);
  lv_label_set_text(watch->dow_label, dow);

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
          watch->touch_start_x = point.x;
          watch->touch_start_y = point.y;
          watch->touch_start_time = now;
        }
    }
  else if (watch->touch_active)
    {
      int dx = point.x - watch->touch_start_x;
      int dy = point.y - watch->touch_start_y;
      int64_t elapsed_ms =
          ai_watch_elapsed_ms(&now, &watch->touch_start_time);

      watch->touch_active = false;

      if (elapsed_ms > AI_WATCH_SWIPE_TIMEOUT_MS)
        {
          return;
        }

      /* Only process gesture on HOME page.
       * Menu page: hex menu container handles all touches.
       * Other pages: back via KEY2 or on-screen buttons.
       */

      if (watch->current_page != AI_WATCH_PAGE_HOME)
        {
          return;
        }

      if (abs(dy) > AI_WATCH_SWIPE_THRESHOLD && abs(dy) > abs(dx))
        {
          if (dy < 0)
            {
              ai_watch_push_page(watch, AI_WATCH_PAGE_APP_LIST);
            }
        }
      else if (abs(dx) < AI_WATCH_SWIPE_THRESHOLD &&
               abs(dy) < AI_WATCH_SWIPE_THRESHOLD)
        {
          ai_watch_push_page(watch, AI_WATCH_PAGE_APP_LIST);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  lv_nuttx_result_t result;
  lv_nuttx_dsc_t info;
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

  printf("AI Watch version %s\n", AI_WATCH_VERSION);
  printf("ai_watch started\n");

  /* Wait for LCD device (mandatory).  Touch will be added later. */

  int retry;

  for (retry = 0; retry < 50; retry++)
    {
      if (access("/dev/lcd0", F_OK) == 0)
        {
          printf("LCD device ready after %d ms\n", retry * 100);
          break;
        }

      usleep(100000);
    }

  if (retry >= 50)
    {
      printf("ERROR: /dev/lcd0 not found after 5s\n");
      return EXIT_FAILURE;
    }

  if (lv_is_initialized())
    {
      printf("ERROR: LVGL is already initialized\n");
      return EXIT_FAILURE;
    }

  lv_init();
  lv_nuttx_dsc_init(&info);

  info.fb_path = "/dev/lcd0";
  info.input_path = NULL;  /* skip touch — added later when ready */

  /* Retry lv_nuttx_init — the driver may still be completing
   * the first DMA transfer when /dev/lcd0 first becomes openable.
   */

  int init_retry;

  for (init_retry = 0; init_retry < 3; init_retry++)
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

  if (result.disp == NULL)
    {
      printf("ERROR: LVGL init failed after %d retries\n", init_retry);
      lv_nuttx_deinit(&result);
      lv_deinit();
      return EXIT_FAILURE;
    }

  printf("LVGL initialized, display ready\n");

  /* Touch may not be registered yet — try now, poll later if not */

  watch.touch_indev = NULL;
  watch.touch_available = false;
  watch.touch_poll_remaining = 30;  /* poll for up to 30s */

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

  if (!watch.touch_available)
    {
      printf("Touch not ready, will poll every 1s\n");
      clock_gettime(CLOCK_MONOTONIC, &watch.touch_next_poll);
      watch.touch_next_poll.tv_sec += 1;
    }

  ai_watch_create_ui(&watch);
  ai_watch_button_initialize(&watch);

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

      idle = lv_timer_handler();
      if (idle == 0 || idle > AI_WATCH_BUTTON_POLL_MS)
        {
          idle = AI_WATCH_BUTTON_POLL_MS;
        }

      usleep(idle * 1000);
    }

  return EXIT_SUCCESS;
}
