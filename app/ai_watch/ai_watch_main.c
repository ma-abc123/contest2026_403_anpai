/****************************************************************************
 * apps/examples/ai_watch/ai_watch_main.c
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

#define AI_WATCH_VERSION            "1.1.3"
#define AI_WATCH_BUTTON_DEVICE      "/dev/buttons"
#define AI_WATCH_BUTTON_KEY2        (1 << 0)
#define AI_WATCH_BUTTON_POLL_MS     10
#define AI_WATCH_BUTTON_RELEASE_MS  80
#define AI_WATCH_RTC_MIN_YEAR       2020

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ai_watch_s
{
  FAR lv_obj_t *time_label;
  FAR lv_obj_t *date_label;
  FAR lv_obj_t *bt_label;
  int button_fd;
  btn_buttonset_t supported_buttons;
  struct timespec button_raw_since;
  struct timespec next_button_poll;
  time_t displayed_second;
  bool button_raw_pressed;
  bool button_armed;
  bool bt_state;
  bool rtc_valid;
  bool rtc_warning_printed;
};

/****************************************************************************
 * Private Functions
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
          watch->bt_state = !watch->bt_state;
          lv_label_set_text(watch->bt_label,
                            watch->bt_state ? "BT: ON" : "BT: OFF");
          printf("KEY2 pressed: BT %s\n",
                 watch->bt_state ? "ON" : "OFF");
        }
    }
  else if (ai_watch_elapsed_ms(now, &watch->button_raw_since) >=
           AI_WATCH_BUTTON_RELEASE_MS)
    {
      watch->button_armed = true;
    }
}

static void ai_watch_time_update(FAR struct ai_watch_s *watch)
{
  char date[32];
  char time[16];
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
}

static void ai_watch_create_ui(FAR struct ai_watch_s *watch)
{
  FAR lv_obj_t *screen = lv_scr_act();
  FAR lv_obj_t *title;

  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

  title = lv_label_create(screen);
  lv_label_set_text(title, "AI Watch");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 55);

  watch->time_label = lv_label_create(screen);
  lv_label_set_text(watch->time_label, "--:--:--");
  lv_obj_set_style_text_color(watch->time_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(watch->time_label, &lv_font_montserrat_24, 0);
  lv_obj_align(watch->time_label, LV_ALIGN_CENTER, 0, -10);

  watch->date_label = lv_label_create(screen);
  lv_label_set_text(watch->date_label, "Date not set");
  lv_obj_set_style_text_color(watch->date_label,
                              lv_color_make(180, 180, 180), 0);
  lv_obj_set_style_text_font(watch->date_label,
                             &lv_font_montserrat_16, 0);
  lv_obj_align_to(watch->date_label, watch->time_label,
                  LV_ALIGN_OUT_BOTTOM_MID, 0, 12);

  watch->bt_label = lv_label_create(screen);
  lv_label_set_text(watch->bt_label, "BT: OFF");
  lv_obj_set_style_text_color(watch->bt_label,
                              lv_color_make(0, 191, 255), 0);
  lv_obj_set_style_text_font(watch->bt_label,
                             &lv_font_montserrat_20, 0);
  lv_obj_align(watch->bt_label, LV_ALIGN_BOTTOM_MID, 0, -55);
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
  info.fb_path = "/dev/lcd0";
  lv_nuttx_init(&info, &result);

  if (result.disp == NULL)
    {
      printf("ERROR: LVGL initialization failed\n");
      lv_deinit();
      return EXIT_FAILURE;
    }

  printf("LVGL initialized, display ready\n");
  ai_watch_create_ui(&watch);
  ai_watch_time_update(&watch);
  ai_watch_button_initialize(&watch);
  printf("UI created; entering main loop\n");

  for (; ; )
    {
      struct timespec now;
      uint32_t idle;

      clock_gettime(CLOCK_MONOTONIC, &now);
      ai_watch_button_update(&watch, &now);
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
