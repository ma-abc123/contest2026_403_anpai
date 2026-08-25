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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/input/buttons.h>

#include <lvgl/lvgl.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AI_WATCH_VERSION            "2.1.0"
#define AI_WATCH_BUTTON_DEVICE      "/dev/buttons"
#define AI_WATCH_BUTTON_KEY2        (1 << 0)
#define AI_WATCH_BUTTON_POLL_MS     10
#define AI_WATCH_BUTTON_RELEASE_MS  80
#define AI_WATCH_RTC_MIN_YEAR       2020
#define AI_WATCH_INPUT_DEVICE       "/dev/input0"

/* Page navigation */

#define AI_WATCH_PAGE_HOME          0
#define AI_WATCH_PAGE_APP_LIST      1
#define AI_WATCH_PAGE_SETTINGS      2
#define AI_WATCH_PAGE_COUNT         3

/* Swipe gesture thresholds */

#define AI_WATCH_SWIPE_THRESHOLD    50
#define AI_WATCH_SWIPE_TIMEOUT_MS   300

/* Theme definitions */

#define AI_WATCH_THEME_DARK         0
#define AI_WATCH_THEME_LIGHT        1
#define AI_WATCH_THEME_BLUE         2
#define AI_WATCH_THEME_COUNT        3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ai_watch_s
{
  /* Display objects - Home page */

  FAR lv_obj_t *home_screen;
  FAR lv_obj_t *time_label;
  FAR lv_obj_t *date_label;
  FAR lv_obj_t *bt_label;

  /* Display objects - App list page */

  FAR lv_obj_t *app_list_screen;
  FAR lv_obj_t *app_icons[4];
  FAR lv_obj_t *app_labels[4];

  /* Display objects - Settings page */

  FAR lv_obj_t *settings_screen;
  FAR lv_obj_t *bt_switch;
  FAR lv_obj_t *theme_roller;
  FAR lv_obj_t *about_label;

  /* Page navigation */

  int current_page;
  FAR lv_obj_t *pages[AI_WATCH_PAGE_COUNT];

  /* Button state */

  int button_fd;
  btn_buttonset_t supported_buttons;
  struct timespec button_raw_since;
  struct timespec next_button_poll;
  bool button_raw_pressed;
  bool button_armed;

  /* Touch/swipe state */

  bool touch_active;
  bool touch_available;
  int touch_start_x;
  int touch_start_y;
  struct timespec touch_start_time;

  /* RTC state */

  time_t displayed_second;
  bool rtc_valid;
  bool rtc_warning_printed;

  /* Application state */

  bool bt_state;
  bool settings_bt_enabled;
  int current_theme;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void ai_watch_navigate_to(FAR struct ai_watch_s *watch, int page);
static void ai_watch_update_bt_state(FAR struct ai_watch_s *watch,
                                     bool new_state);
static void ai_watch_update_theme(FAR struct ai_watch_s *watch, int theme);
static void ai_watch_create_home_page(FAR struct ai_watch_s *watch);
static void ai_watch_create_app_list_page(FAR struct ai_watch_s *watch);
static void ai_watch_create_settings_page(FAR struct ai_watch_s *watch);

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
  if (theme < 0 || theme >= AI_WATCH_THEME_COUNT ||
      theme == watch->current_theme)
    {
      return;
    }

  watch->current_theme = theme;
  printf("Theme changed to %d\n", theme);

  /* Update all pages with new theme colors */

  lv_color_t bg = ai_watch_theme_bg(theme);
  lv_color_t text = ai_watch_theme_text(theme);
  lv_color_t accent = ai_watch_theme_accent(theme);
  lv_color_t secondary = ai_watch_theme_secondary(theme);
  lv_color_t btn_bg = ai_watch_theme_btn_bg(theme);

  /* Home page - title, time, date, bt label, hint */

  lv_obj_set_style_bg_color(watch->home_screen, bg, 0);
  lv_obj_set_style_text_color(
      lv_obj_get_child(watch->home_screen, 0), text, 0);
  lv_obj_set_style_text_color(watch->time_label, text, 0);
  lv_obj_set_style_text_color(watch->date_label, secondary, 0);
  lv_obj_set_style_text_color(watch->bt_label, accent, 0);

  /* Find and update home page hint (last child) */

  uint32_t home_child_count = lv_obj_get_child_count(watch->home_screen);
  if (home_child_count > 0)
    {
      lv_obj_set_style_text_color(
          lv_obj_get_child(watch->home_screen, home_child_count - 1),
          secondary, 0);
    }

  /* App list page - title, grid, all buttons, hint */

  lv_obj_set_style_bg_color(watch->app_list_screen, bg, 0);
  lv_obj_set_style_text_color(
      lv_obj_get_child(watch->app_list_screen, 0), text, 0);

  /* Update grid and all app buttons */

  uint32_t app_child_count =
      lv_obj_get_child_count(watch->app_list_screen);
  if (app_child_count > 1)
    {
      FAR lv_obj_t *grid =
          lv_obj_get_child(watch->app_list_screen, 1);

      lv_obj_set_style_bg_color(grid, bg, 0);
      uint32_t grid_child_count = lv_obj_get_child_count(grid);
      uint32_t j;

      for (j = 0; j < grid_child_count; j++)
        {
          FAR lv_obj_t *btn = lv_obj_get_child(grid, j);

          lv_obj_set_style_bg_color(btn, btn_bg, 0);

          /* Update icon and text labels inside button */

          uint32_t btn_child_count = lv_obj_get_child_count(btn);
          uint32_t k;

          for (k = 0; k < btn_child_count; k++)
            {
              lv_obj_set_style_text_color(
                  lv_obj_get_child(btn, k), text, 0);
            }
        }
    }

  /* Find and update app list hint (last child) */

  if (app_child_count > 0)
    {
      lv_obj_set_style_text_color(
          lv_obj_get_child(watch->app_list_screen, app_child_count - 1),
          secondary, 0);
    }

  /* Settings page - title, labels, about, hint */

  lv_obj_set_style_bg_color(watch->settings_screen, bg, 0);

  uint32_t settings_child_count =
      lv_obj_get_child_count(watch->settings_screen);
  uint32_t i;

  for (i = 0; i < settings_child_count; i++)
    {
      FAR lv_obj_t *child =
          lv_obj_get_child(watch->settings_screen, i);

      /* Update all label colors */

      if (child == watch->about_label)
        {
          lv_obj_set_style_text_color(child, secondary, 0);
        }
      else if (child != watch->bt_switch &&
               child != watch->theme_roller)
        {
          lv_obj_set_style_text_color(child, text, 0);
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

  /* Update home page BT label */

  lv_label_set_text(watch->bt_label,
                    watch->bt_state ? "BT: ON" : "BT: OFF");

  /* Update settings page switch if it exists */

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

          /* KEY2 acts as back button */

          if (watch->current_page == AI_WATCH_PAGE_HOME)
            {
              /* On home page, toggle BT */

              ai_watch_update_bt_state(watch, !watch->bt_state);
              printf("KEY2 pressed: BT %s\n",
                     watch->bt_state ? "ON" : "OFF");
            }
          else
            {
              /* On other pages, go back to home */

              ai_watch_navigate_to(watch, AI_WATCH_PAGE_HOME);
              printf("KEY2 pressed: Back to home\n");
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
  char time[16];
  char about[128];
  struct timespec ts;
  struct tm tm;

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
          lv_label_set_text(watch->time_label, "--:--:--");
          lv_label_set_text(watch->date_label, "Date not set");
          watch->rtc_valid = false;

          /* Update about label with correct RTC status */

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
  strftime(time, sizeof(time), "%H:%M:%S", &tm);
  strftime(date, sizeof(date), "%Y-%m-%d", &tm);
  lv_label_set_text(watch->time_label, time);
  lv_label_set_text(watch->date_label, date);

  /* Update about label with current time */

  snprintf(about, sizeof(about),
           "AI Watch v" AI_WATCH_VERSION "\n"
           "Board: SF32LB52\n"
           "Touch: %s\n"
           "RTC: %s",
           watch->touch_available ? "Active" : "Unavailable",
           time);
  lv_label_set_text(watch->about_label, about);
}

/****************************************************************************
 * Private Functions - Page Navigation
 ****************************************************************************/

static void ai_watch_navigate_to(FAR struct ai_watch_s *watch, int page)
{
  if (page < 0 || page >= AI_WATCH_PAGE_COUNT ||
      page == watch->current_page)
    {
      return;
    }

  /* Switch to target screen */

  watch->current_page = page;
  if (watch->pages[page] != NULL)
    {
      lv_scr_load_anim(watch->pages[page], LV_SCR_LOAD_ANIM_NONE, 0, 0,
                       false);
      printf("Navigated to page %d\n", page);
    }
}

/****************************************************************************
 * Private Functions - UI Callbacks
 ****************************************************************************/

static void ai_watch_app_icon_cb(lv_event_t *e)
{
  FAR lv_obj_t *btn = lv_event_get_target(e);
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);
  int index = (int)(intptr_t)lv_obj_get_user_data(btn);

  printf("App %d tapped\n", index);

  switch (index)
    {
      case 0: /* Exercise */
      case 1: /* Timer */
      case 2: /* Reminder */
        {
          FAR lv_obj_t *msgbox = lv_msgbox_create(NULL);

          lv_msgbox_add_title(msgbox, "Coming Soon");
          lv_msgbox_add_text(msgbox, "This app is under development.");
          lv_msgbox_add_close_button(msgbox);
          break;
        }
      case 3: /* Settings */
        {
          ai_watch_navigate_to(watch, AI_WATCH_PAGE_SETTINGS);
          break;
        }
      default:
        break;
    }
}

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

static void ai_watch_back_cb(lv_event_t *e)
{
  FAR struct ai_watch_s *watch = lv_event_get_user_data(e);

  if (watch->current_page == AI_WATCH_PAGE_SETTINGS ||
      watch->current_page == AI_WATCH_PAGE_APP_LIST)
    {
      ai_watch_navigate_to(watch, AI_WATCH_PAGE_HOME);
    }
}

/****************************************************************************
 * Private Functions - UI Creation
 ****************************************************************************/

static void ai_watch_create_home_page(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen = lv_obj_create(NULL);
  FAR lv_obj_t *title;
  FAR lv_obj_t *hint;

  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(watch->current_theme),
                            0);
  watch->home_screen = screen;
  watch->pages[AI_WATCH_PAGE_HOME] = screen;

  /* Title */

  title = lv_label_create(screen);
  lv_label_set_text(title, "AI Watch");
  lv_obj_set_style_text_color(title,
                              ai_watch_theme_text(watch->current_theme), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 55);

  /* Time display */

  watch->time_label = lv_label_create(screen);
  lv_label_set_text(watch->time_label, "--:--:--");
  lv_obj_set_style_text_color(watch->time_label,
                              ai_watch_theme_text(watch->current_theme), 0);
  lv_obj_set_style_text_font(watch->time_label, &lv_font_montserrat_24, 0);
  lv_obj_align(watch->time_label, LV_ALIGN_CENTER, 0, -10);

  /* Date display */

  watch->date_label = lv_label_create(screen);
  lv_label_set_text(watch->date_label, "Date not set");
  lv_obj_set_style_text_color(watch->date_label,
                              ai_watch_theme_secondary(watch->current_theme),
                              0);
  lv_obj_set_style_text_font(watch->date_label, &lv_font_montserrat_16, 0);
  lv_obj_align_to(watch->date_label, watch->time_label,
                  LV_ALIGN_OUT_BOTTOM_MID, 0, 12);

  /* Bluetooth status */

  watch->bt_label = lv_label_create(screen);
  lv_label_set_text(watch->bt_label, "BT: OFF");
  lv_obj_set_style_text_color(
      watch->bt_label,
      ai_watch_theme_accent(watch->current_theme), 0);
  lv_obj_set_style_text_font(watch->bt_label, &lv_font_montserrat_20, 0);
  lv_obj_align(watch->bt_label, LV_ALIGN_BOTTOM_MID, 0, -55);

  /* Navigation hint */

  hint = lv_label_create(screen);
  lv_label_set_text(hint, "Tap or swipe up for apps");
  lv_obj_set_style_text_color(hint,
                              ai_watch_theme_secondary(watch->current_theme),
                              0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

  lv_obj_add_event_cb(screen, ai_watch_back_cb, LV_EVENT_CLICKED, watch);
}

static void ai_watch_create_app_list_page(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen = lv_obj_create(NULL);
  FAR lv_obj_t *title;
  FAR lv_obj_t *grid;
  static const char *app_names[] =
  {
    "Exercise", "Timer", "Reminder", "Settings"
  };
  static const char *app_icons_text[] =
  {
    LV_SYMBOL_PLAY, LV_SYMBOL_PAUSE,
    LV_SYMBOL_BELL, LV_SYMBOL_SETTINGS
  };
  int i;

  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(watch->current_theme),
                            0);
  watch->app_list_screen = screen;
  watch->pages[AI_WATCH_PAGE_APP_LIST] = screen;

  /* Title */

  title = lv_label_create(screen);
  lv_label_set_text(title, "Apps");
  lv_obj_set_style_text_color(title,
                              ai_watch_theme_text(watch->current_theme), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

  /* Create 2x2 grid of app icons */

  grid = lv_obj_create(screen);
  lv_obj_set_size(grid, 200, 200);
  lv_obj_align(grid, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_style_bg_color(grid, ai_watch_theme_bg(watch->current_theme),
                            0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 10, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  for (i = 0; i < 4; i++)
    {
      FAR lv_obj_t *icon_btn = lv_btn_create(grid);
      FAR lv_obj_t *icon_label;
      FAR lv_obj_t *name_label;

      lv_obj_set_size(icon_btn, 80, 80);
      lv_obj_set_style_bg_color(icon_btn,
                                ai_watch_theme_btn_bg(watch->current_theme),
                                0);
      lv_obj_set_style_radius(icon_btn, 12, 0);

      lv_obj_set_user_data(icon_btn, (void *)(intptr_t)i);

      /* Icon symbol */

      icon_label = lv_label_create(icon_btn);
      lv_label_set_text(icon_label, app_icons_text[i]);
      lv_obj_set_style_text_color(icon_label,
                                  ai_watch_theme_text(watch->current_theme),
                                  0);
      lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_28, 0);
      lv_obj_align(icon_label, LV_ALIGN_CENTER, 0, -10);

      /* App name */

      name_label = lv_label_create(icon_btn);
      lv_label_set_text(name_label, app_names[i]);
      lv_obj_set_style_text_color(name_label,
                                  ai_watch_theme_text(watch->current_theme),
                                  0);
      lv_obj_set_style_text_font(name_label, &lv_font_montserrat_16, 0);
      lv_obj_align(name_label, LV_ALIGN_CENTER, 0, 15);

      lv_obj_add_event_cb(icon_btn, ai_watch_app_icon_cb,
                          LV_EVENT_CLICKED, watch);
    }

  /* Back hint */

  FAR lv_obj_t *hint = lv_label_create(screen);

  lv_label_set_text(hint, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_color(hint,
                              ai_watch_theme_secondary(watch->current_theme),
                              0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void ai_watch_create_settings_page(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen = lv_obj_create(NULL);
  FAR lv_obj_t *title;
  FAR lv_obj_t *bt_label;
  FAR lv_obj_t *theme_label;
  FAR lv_obj_t *hint;

  lv_obj_set_style_bg_color(screen, ai_watch_theme_bg(watch->current_theme),
                            0);
  watch->settings_screen = screen;
  watch->pages[AI_WATCH_PAGE_SETTINGS] = screen;

  /* Title */

  title = lv_label_create(screen);
  lv_label_set_text(title, "Settings");
  lv_obj_set_style_text_color(title,
                              ai_watch_theme_text(watch->current_theme), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

  /* Bluetooth switch */

  bt_label = lv_label_create(screen);
  lv_label_set_text(bt_label, "Bluetooth");
  lv_obj_set_style_text_color(bt_label,
                              ai_watch_theme_text(watch->current_theme), 0);
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

  /* Theme selector */

  theme_label = lv_label_create(screen);
  lv_label_set_text(theme_label, "Theme");
  lv_obj_set_style_text_color(theme_label,
                              ai_watch_theme_text(watch->current_theme), 0);
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

  /* About section */

  watch->about_label = lv_label_create(screen);
  lv_label_set_text(watch->about_label,
                    "AI Watch v" AI_WATCH_VERSION "\n"
                    "Board: SF32LB52\n"
                    "Touch: Checking...\n"
                    "RTC: Checking...");
  lv_obj_set_style_text_color(watch->about_label,
                              ai_watch_theme_secondary(watch->current_theme),
                              0);
  lv_obj_set_style_text_font(watch->about_label, &lv_font_montserrat_14, 0);
  lv_obj_align(watch->about_label, LV_ALIGN_CENTER, 0, 50);

  /* Back hint */

  hint = lv_label_create(screen);
  lv_label_set_text(hint, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_color(hint,
                              ai_watch_theme_secondary(watch->current_theme),
                              0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

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
  /* Create all pages */

  ai_watch_create_home_page(watch);
  ai_watch_create_app_list_page(watch);
  ai_watch_create_settings_page(watch);

  /* Set home as active page */

  watch->current_page = AI_WATCH_PAGE_HOME;
  lv_scr_load_anim(watch->home_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);

  printf("UI created with 3 pages\n");
}

/****************************************************************************
 * Private Functions - Touch Input
 ****************************************************************************/

static void ai_watch_touch_update(FAR struct ai_watch_s *watch)
{
  FAR lv_indev_t *indev = lv_indev_get_next(NULL);
  lv_indev_state_t state;
  lv_point_t point;
  struct timespec now;

  if (indev == NULL || !watch->touch_available)
    {
      return;
    }

  state = lv_indev_get_state(indev);
  lv_indev_get_point(indev, &point);
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

      if (abs(dy) > AI_WATCH_SWIPE_THRESHOLD && abs(dy) > abs(dx))
        {
          if (dy < 0)
            {
              if (watch->current_page == AI_WATCH_PAGE_HOME)
                {
                  ai_watch_navigate_to(watch, AI_WATCH_PAGE_APP_LIST);
                }
            }
          else
            {
              if (watch->current_page == AI_WATCH_PAGE_APP_LIST ||
                  watch->current_page == AI_WATCH_PAGE_SETTINGS)
                {
                  ai_watch_navigate_to(watch, AI_WATCH_PAGE_HOME);
                }
            }
        }
      else if (abs(dx) > AI_WATCH_SWIPE_THRESHOLD && abs(dx) > abs(dy))
        {
          if (dx > 0)
            {
              if (watch->current_page == AI_WATCH_PAGE_APP_LIST ||
                  watch->current_page == AI_WATCH_PAGE_SETTINGS)
                {
                  ai_watch_navigate_to(watch, AI_WATCH_PAGE_HOME);
                }
            }
        }
      else if (abs(dx) < AI_WATCH_SWIPE_THRESHOLD &&
               abs(dy) < AI_WATCH_SWIPE_THRESHOLD)
        {
          if (watch->current_page == AI_WATCH_PAGE_HOME)
            {
              ai_watch_navigate_to(watch, AI_WATCH_PAGE_APP_LIST);
            }
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
    .touch_available = false,
  };

  printf("AI Watch version %s\n", AI_WATCH_VERSION);
  printf("ai_watch started\n");

  if (lv_is_initialized())
    {
      printf("ERROR: LVGL is already initialized\n");
      return EXIT_FAILURE;
    }

  lv_init();
  lv_nuttx_dsc_init(&info);

  /* Configure display and touch input */

  info.fb_path = "/dev/lcd0";
  info.input_path = AI_WATCH_INPUT_DEVICE;

  lv_nuttx_init(&info, &result);

  if (result.disp == NULL)
    {
      printf("ERROR: LVGL initialization failed\n");
      lv_deinit();
      return EXIT_FAILURE;
    }

  printf("LVGL initialized, display ready\n");

  /* Check if touch input is actually available */

  if (result.indev != NULL)
    {
      watch.touch_available = true;
      printf("Touch input initialized: %s\n", AI_WATCH_INPUT_DEVICE);
    }
  else
    {
      watch.touch_available = false;
      printf("WARNING: Touch input not available\n");
    }

  ai_watch_create_ui(&watch);
  ai_watch_button_initialize(&watch);

  /* Initialize About label with correct touch and RTC status immediately */

  ai_watch_init_about(&watch);
  ai_watch_time_update(&watch);

  printf("UI created; entering main loop\n");

  for (; ; )
    {
      struct timespec now;
      uint32_t idle;

      clock_gettime(CLOCK_MONOTONIC, &now);
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
