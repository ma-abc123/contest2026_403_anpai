/****************************************************************************
 * apps/ai_watch/ai_watch_ble.c
 *
 * Application-side BLE protocol logic.
 *
 * The zblue integration lives in the BSP bridge (sf32lb52_ble_bridge.c);
 * this file only talks to the stable wrapper in ai_watch_ble_bsp.h and
 * owns:
 *   - TimeSync consumption: clock_settime (UTC) + display timezone
 *   - Command frame parsing and the reminder store
 *   - Status / DataUpload notifications
 *
 * Everything here runs on the application main loop; BLE callbacks in the
 * bridge only enqueue data. Single-threaded, therefore no locking.
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ai_watch_ble.h"
#include "ai_watch_ble_bsp.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct ai_watch_reminder_store_s g_reminders;
static int16_t g_tz_offset_min;         /* minutes east of UTC (display) */
static uint32_t g_last_sync_utc;        /* UTC of last accepted sync */

/****************************************************************************
 * Private Functions - Status / DataUpload notifications
 ****************************************************************************/

static void ai_ble_notify_status(void)
{
  uint8_t value[AI_WATCH_BLE_STATUS_SIZE];
  uint32_t sync = g_last_sync_utc;

  value[0] = AI_WATCH_BLE_PROTOCOL_VERSION;
  value[1] = (ai_watch_ble_get_state() ==
              AI_WATCH_BLE_BSP_CONNECTED) ? 1 : 0;
  value[2] = (uint8_t)(sync >> 0);
  value[3] = (uint8_t)(sync >> 8);
  value[4] = (uint8_t)(sync >> 16);
  value[5] = (uint8_t)(sync >> 24);

  ai_watch_ble_bsp_notify(AI_WATCH_BLE_CHR_STATUS, value, sizeof(value));
}

/****************************************************************************
 * Private Functions - TimeSync
 ****************************************************************************/

static void ai_ble_apply_timesync(
    FAR const struct ai_watch_ble_bsp_timesync_s *sync)
{
  struct timespec ts;

  ts.tv_sec = (time_t)sync->utc_seconds;
  ts.tv_nsec = 0;

  if (clock_settime(CLOCK_REALTIME, &ts) < 0)
    {
      printf("BLE: clock_settime failed: %d\n", errno);
      return;
    }

  g_tz_offset_min = sync->tz_offset_min;
  g_last_sync_utc = sync->utc_seconds;

  printf("BLE: time synced, utc=%lu tz=%+d min\n",
         (unsigned long)sync->utc_seconds, (int)sync->tz_offset_min);

  ai_ble_notify_status();
}

/****************************************************************************
 * Private Functions - Command frame parsing
 *
 * Frame: [version(1)][cmd_type(1)][id(1)][flags(1)][timestamp(4)]
 *        [title_len(1)][title(N)]
 *
 * Runs on the main loop thread; the reminder store is only touched here.
 ****************************************************************************/

static FAR struct ai_watch_reminder_s *ai_ble_find_slot(uint8_t id)
{
  int i;

  for (i = 0; i < AI_WATCH_REMINDER_MAX; i++)
    {
      if (g_reminders.items[i].id == id)
        {
          return &g_reminders.items[i];
        }
    }

  return NULL;
}

static FAR struct ai_watch_reminder_s *ai_ble_free_slot(void)
{
  int i;

  for (i = 0; i < AI_WATCH_REMINDER_MAX; i++)
    {
      if (g_reminders.items[i].id == 0)
        {
          return &g_reminders.items[i];
        }
    }

  return NULL;
}

static bool ai_ble_cmd_reminder(FAR const uint8_t *data, size_t len)
{
  FAR struct ai_watch_reminder_s *slot;
  uint32_t timestamp;
  uint8_t title_len;

  if (len < AI_WATCH_BLE_CMD_HDR_LEN)
    {
      printf("BLE: reminder frame too short (%u)\n", (unsigned int)len);
      return false;
    }

  title_len = data[8];
  if (title_len > AI_WATCH_REMINDER_TITLE_MAX)
    {
      printf("BLE: reminder title too long (%u)\n", title_len);
      return false;
    }

  if (len != (size_t)AI_WATCH_BLE_CMD_HDR_LEN + title_len)
    {
      printf("BLE: reminder frame length mismatch (%u < %u+%u)\n",
             (unsigned int)len, AI_WATCH_BLE_CMD_HDR_LEN,
             title_len);
      return false;
    }

  memcpy(&timestamp, &data[4], sizeof(timestamp));

  /* id 0 is reserved to mark empty slots */

  if (data[2] == 0)
    {
      printf("BLE: reminder id 0 reserved\n");
      return false;
    }

  slot = ai_ble_find_slot(data[2]);
  if (slot == NULL)
    {
      slot = ai_ble_free_slot();
    }

  if (slot == NULL)
    {
      printf("BLE: reminder store full\n");
      return false;
    }

  slot->id = data[2];
  slot->flags = data[3];
  slot->timestamp = timestamp;
  memcpy(slot->title, &data[9], title_len);
  slot->title[title_len] = '\0';

  g_reminders.pending = true;
  printf("BLE: reminder[%u] \"%s\" flags=0x%02x\n",
         data[2], slot->title, data[3]);
  return true;
}

static bool ai_ble_parse_command(FAR const uint8_t *data, size_t len)
{
  if (len < 2)
    {
      printf("BLE: command too short (%u)\n", (unsigned int)len);
      return false;
    }

  if (data[0] != AI_WATCH_BLE_PROTOCOL_VERSION)
    {
      printf("BLE: command version %u rejected\n", data[0]);
      return false;
    }

  switch (data[1])
    {
      case AI_WATCH_BLE_CMD_REMINDER:
      case AI_WATCH_BLE_CMD_NOTIFICATION:
        return ai_ble_cmd_reminder(data, len);

      case AI_WATCH_BLE_CMD_CLEAR:
        memset(&g_reminders, 0, sizeof(g_reminders));
        g_reminders.pending = true;
        printf("BLE: reminders cleared\n");
        return true;

      default:
        printf("BLE: unknown command 0x%02x\n", data[1]);
        return false;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ai_watch_ble_init
 ****************************************************************************/

int ai_watch_ble_init(void)
{
  memset(&g_reminders, 0, sizeof(g_reminders));
  g_tz_offset_min = 0;
  g_last_sync_utc = 0;

  return ai_watch_ble_bsp_start();
}

/****************************************************************************
 * Name: ai_watch_ble_get_state
 ****************************************************************************/

enum ai_watch_ble_bsp_state_e ai_watch_ble_get_state(void)
{
  return ai_watch_ble_bsp_get_state();
}

/****************************************************************************
 * Name: ai_watch_ble_get_status_text
 ****************************************************************************/

FAR const char *ai_watch_ble_get_status_text(
    enum ai_watch_ble_bsp_state_e state)
{
  switch (state)
    {
      case AI_WATCH_BLE_BSP_OFF:
        return "BLE: OFF";
      case AI_WATCH_BLE_BSP_INIT_FAILED:
        return "BLE: ERR";
      case AI_WATCH_BLE_BSP_ADVERTISING:
        return "BLE: ADV";
      case AI_WATCH_BLE_BSP_CONNECTED:
        return "BLE: ON";
      case AI_WATCH_BLE_BSP_DISCONNECTED:
        return "BLE: LOST";
      default:
        return "BLE: ???";
    }
}

/****************************************************************************
 * Name: ai_watch_ble_process
 ****************************************************************************/

void ai_watch_ble_process(void)
{
  struct ai_watch_ble_bsp_timesync_s sync;
  uint8_t cmd[AI_WATCH_BLE_CMD_MAX];
  size_t len;

  /* Deliver a pending validated TimeSync: RTC gets UTC, the timezone
   * offset is remembered for display only.
   */

  if (ai_watch_ble_bsp_take_timesync(&sync))
    {
      ai_ble_apply_timesync(&sync);
    }

  /* Drain all queued Command frames */

  while ((len = ai_watch_ble_bsp_take_command(cmd, sizeof(cmd))) > 0)
    {
      ai_ble_parse_command(cmd, len);
    }

  /* Auto-reconnect: after a link loss, retry re-advertising once per
   * second from this (application) thread until it succeeds - the
   * bridge cannot run the HCI command sequence reliably from its RX
   * context, and the host does not auto-resume ONE_TIME advertising.
   */

  if (ai_watch_ble_get_state() == AI_WATCH_BLE_BSP_DISCONNECTED)
    {
      static struct timespec next_retry;
      struct timespec now;

      clock_gettime(CLOCK_MONOTONIC, &now);

      if (now.tv_sec >= next_retry.tv_sec)
        {
          ai_watch_ble_bsp_resume_advertising();
          next_retry = now;
          next_retry.tv_sec += 1;
        }
    }
}

/****************************************************************************
 * Name: ai_watch_ble_set_enabled
 ****************************************************************************/

void ai_watch_ble_set_enabled(bool enabled)
{
  ai_watch_ble_bsp_set_enabled(enabled);
}

/****************************************************************************
 * Name: ai_watch_ble_get_tz_offset_min / ai_watch_ble_get_last_sync
 ****************************************************************************/

int16_t ai_watch_ble_get_tz_offset_min(void)
{
  return g_tz_offset_min;
}

uint32_t ai_watch_ble_get_last_sync(void)
{
  return g_last_sync_utc;
}

/****************************************************************************
 * Name: ai_watch_ble_localtime
 ****************************************************************************/

FAR struct tm *ai_watch_ble_localtime(time_t utc, FAR struct tm *tm)
{
  time_t local;

  if (tm == NULL)
    {
      return NULL;
    }

  local = utc + (time_t)g_tz_offset_min * 60;
  return gmtime_r(&local, tm);
}

/****************************************************************************
 * Name: ai_watch_ble_get_reminders
 ****************************************************************************/

FAR struct ai_watch_reminder_store_s *ai_watch_ble_get_reminders(void)
{
  return &g_reminders;
}

/****************************************************************************
 * Name: ai_watch_ble_send_sensor_data
 *
 * Description:
 *   Notify one sample on DataUpload:
 *   [version(1)][sensor_type(1)][payload(N)].
 *
 ****************************************************************************/

int ai_watch_ble_send_sensor_data(uint8_t sensor_type,
                                  FAR const void *data,
                                  uint8_t data_len)
{
  uint8_t buf[AI_WATCH_BLE_DATAUPLOAD_HDR + 16];

  if (data == NULL ||
      (size_t)data_len + AI_WATCH_BLE_DATAUPLOAD_HDR > sizeof(buf))
    {
      return -EINVAL;
    }

  buf[0] = AI_WATCH_BLE_PROTOCOL_VERSION;
  buf[1] = sensor_type;
  memcpy(&buf[AI_WATCH_BLE_DATAUPLOAD_HDR], data, data_len);

  return ai_watch_ble_bsp_notify(AI_WATCH_BLE_CHR_DATAUPLOAD, buf,
                                 (uint16_t)(AI_WATCH_BLE_DATAUPLOAD_HDR +
                                            data_len));
}
