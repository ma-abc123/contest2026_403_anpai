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
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ai_watch_ble.h"
#include "ai_watch_ble_bsp.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Outgoing sensor-frame queue (see ai_watch_ble_post). Depth covers a
 * couple of seconds of recording frames at the worker's effective
 * radio-paced drain rate.
 */

#define AI_WATCH_BLE_TXQ_LEN        48
#define AI_WATCH_BLE_TXQ_DATA_MAX   30
#define AI_WATCH_BLE_TX_BACKOFF_MS  2000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ai_watch_ble_txq_entry_s
{
  uint8_t sensor_type;
  uint8_t len;
  uint8_t data[AI_WATCH_BLE_TXQ_DATA_MAX];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct ai_watch_reminder_store_s g_reminders;
static int16_t g_tz_offset_min;         /* minutes east of UTC (display) */
static uint32_t g_last_sync_utc;        /* UTC of last accepted sync */

/* Last AI result text + pending AI countdown request */

static struct ai_watch_ai_result_s g_ai_result;
static struct ai_watch_ai_timer_req_s g_ai_timer_req;

/* Arrivals waiting to be surfaced as the UI alert banner */

static struct ai_watch_ble_alert_s
    g_alerts[AI_WATCH_BLE_ALERT_RING_LEN];
static uint8_t g_alert_head;            /* next write slot */
static uint8_t g_alert_count;           /* entries waiting */

/* Asynchronous DataUpload sender. bt_gatt_indicate() blocks for up to a
 * connection interval (~100-150 ms) per frame on this port, so every
 * periodic upload goes through the worker thread instead of the LVGL
 * loop - otherwise touch sampling dies at recording rates (seen on
 * 4.8.0: every 4th REC sample showed a ~120 ms main-loop stall).
 *
 * Single producer (the LVGL main loop; ble_process/motion/UI all run
 * there), single consumer (tx_worker). Plain volatile indices are
 * sufficient on this single-core target.
 */

static struct ai_watch_ble_txq_entry_s
    g_txq[AI_WATCH_BLE_TXQ_LEN];
static volatile uint8_t g_txq_head;
static volatile uint8_t g_txq_tail;
static uint32_t g_txq_dropped;
static pthread_t g_tx_thread;
static volatile bool g_tx_thread_ok;

/****************************************************************************
 * Private Functions - DataUpload TX worker
 ****************************************************************************/

static FAR void *ai_ble_tx_worker(FAR void *arg)
{
  (void)arg;

  for (;;)
    {
      if (g_txq_tail == g_txq_head)
        {
          usleep(20 * 1000);
          continue;
        }

      FAR struct ai_watch_ble_txq_entry_s *e = &g_txq[g_txq_tail];
      uint8_t next = (g_txq_tail + 1u) % AI_WATCH_BLE_TXQ_LEN;
      int ret;

      ret = ai_watch_ble_send_sensor_data(e->sensor_type,
                                          e->data, e->len);

      /* Consume the entry either way: drops beat re-sending stale
       * samples. A failure (peer not yet subscribed, transient radio
       * state) just costs a backoff sleep - without it the "Device is
       * not subscribed" path would retry at loop rate and flood the
       * console (119 warnings on the 4.8.0 log).
       */

      g_txq_tail = next;

      if (ret != 0)
        {
          usleep(AI_WATCH_BLE_TX_BACKOFF_MS * 1000);
        }
      else
        {
          usleep(30 * 1000);   /* pace: radio caps the real rate anyway */
        }
    }

  return NULL;
}

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
 * Private Functions - Alert ring
 *
 * Filled from the command parser (main-loop thread), drained by the UI
 * (also main-loop thread): single producer/consumer, no locking needed.
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

static void ai_ble_alert_push(uint8_t type, uint8_t id, uint32_t timestamp,
                              FAR const char *title)
{
  FAR struct ai_watch_ble_alert_s *alert;

  if (g_alert_count >= AI_WATCH_BLE_ALERT_RING_LEN)
    {
      /* Ring full: drop the oldest so the newest state stays visible */

      g_alert_head = (g_alert_head + 1) % AI_WATCH_BLE_ALERT_RING_LEN;
      g_alert_count--;
    }

  alert = &g_alerts[(g_alert_head + g_alert_count) %
                    AI_WATCH_BLE_ALERT_RING_LEN];
  alert->type = type;
  alert->id = id;
  alert->timestamp = timestamp;
  strlcpy(alert->title, title, sizeof(alert->title));
  g_alert_count++;
}

bool ai_watch_ble_take_alert(FAR struct ai_watch_ble_alert_s *out)
{
  FAR struct ai_watch_ble_alert_s *alert;

  if (out == NULL || g_alert_count == 0)
    {
      return false;
    }

  alert = &g_alerts[g_alert_head];
  memcpy(out, alert, sizeof(*out));

  g_alert_head = (g_alert_head + 1) % AI_WATCH_BLE_ALERT_RING_LEN;
  g_alert_count--;
  return true;
}

/****************************************************************************
 * Private Functions - Command frame parsing
 *
 * Frame: [version(1)][cmd_type(1)][id(1)][flags(1)][timestamp(4)]
 *        [title_len(1)][title(N)]
 *
 * Runs on the main loop thread; the reminder store is only touched here.
 ****************************************************************************/

static void ai_ble_recount(void)
{
  int i;
  uint8_t n = 0;

  for (i = 0; i < AI_WATCH_REMINDER_MAX; i++)
    {
      if (g_reminders.items[i].id != 0)
        {
          n++;
        }
    }

  g_reminders.count = n;
}

static bool ai_ble_cmd_reminder(uint8_t cmd_type,
                                FAR const uint8_t *data, size_t len)
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
  slot->type = cmd_type;
  slot->timestamp = timestamp;
  memcpy(slot->title, &data[9], title_len);
  slot->title[title_len] = '\0';

  /* Due-time banner bookkeeping: a reminder whose time has already
   * passed (or that carries no time) is announced by the arrival
   * banner below and must not fire a second alert. A future time
   * arms ai_ble_check_due().
   */

  slot->due_alerted =
    (cmd_type != AI_WATCH_BLE_CMD_REMINDER) ||
    (timestamp == 0) ||
    ((uint32_t)time(NULL) >= timestamp);

  ai_ble_recount();
  g_reminders.pending = true;
  ai_ble_alert_push(cmd_type, slot->id, slot->timestamp, slot->title);
  printf("BLE: %s[%u] \"%s\" flags=0x%02x\n",
         (cmd_type == AI_WATCH_BLE_CMD_NOTIFICATION) ?
         "notification" : "reminder",
         slot->id, slot->title, slot->flags);
  return true;
}

/****************************************************************************
 * Private Functions - AI command frames (AI_TEXT / AI_TIMER)
 *
 * AI_TEXT:    [version][0x04][id][flags][timestamp(4)][len][text]
 *             text is UTF-8, up to AI_WATCH_AI_TEXT_MAX bytes. The full
 *             text is kept for the AI page; the alert banner shows the
 *             truncated copy.
 *
 * AI_TIMER:   [version][0x05][id][flags][duration_s(4)][len][label]
 *             duration_s replaces the usual UTC timestamp field; label
 *             is a short UTF-8 name. One request at a time - a newer
 *             frame replaces a pending one.
 ****************************************************************************/

static bool ai_ble_cmd_ai_text(FAR const uint8_t *data, size_t len)
{
  uint32_t timestamp;
  uint8_t title_len;

  if (len < AI_WATCH_BLE_CMD_HDR_LEN)
    {
      printf("BLE: ai_text frame too short (%u)\n", (unsigned int)len);
      return false;
    }

  title_len = data[8];
  if (title_len > AI_WATCH_AI_TEXT_MAX)
    {
      printf("BLE: ai_text too long (%u)\n", title_len);
      return false;
    }

  if (len != (size_t)AI_WATCH_BLE_CMD_HDR_LEN + title_len)
    {
      printf("BLE: ai_text length mismatch\n");
      return false;
    }

  if (data[2] == 0)
    {
      printf("BLE: ai_text id 0 reserved\n");
      return false;
    }

  memcpy(&timestamp, &data[4], sizeof(timestamp));

  memcpy(g_ai_result.text, &data[9], title_len);
  g_ai_result.text[title_len] = '\0';
  g_ai_result.timestamp = timestamp;
  g_ai_result.id = data[2];
  g_ai_result.pending = true;

  /* Banner copy: the banner title holds only 24 bytes; trim the copy
   * back to a UTF-8 character boundary so CJK text is not cut mid-char.
   */

  {
    char banner[AI_WATCH_REMINDER_TITLE_MAX + 1];
    uint8_t n = AI_WATCH_REMINDER_TITLE_MAX;

    if (title_len < n)
      {
        n = title_len;
      }

    memcpy(banner, &data[9], n);

    while (n > 0 && (banner[n - 1] & 0xc0) == 0x80)
      {
        n--;
      }

    if (n > 0 && (banner[n - 1] & 0xc0) == 0xc0)
      {
        n--;
      }

    banner[n] = '\0';
    ai_ble_alert_push(AI_WATCH_BLE_CMD_AI_TEXT, g_ai_result.id,
                      g_ai_result.timestamp, banner);
  }

  printf("BLE: ai_text[%u] %u bytes\n", g_ai_result.id, title_len);
  return true;
}

static bool ai_ble_cmd_ai_timer(FAR const uint8_t *data, size_t len)
{
  FAR struct ai_watch_ai_timer_req_s *req = &g_ai_timer_req;
  uint32_t duration;
  uint8_t title_len;

  if (len < AI_WATCH_BLE_CMD_HDR_LEN)
    {
      printf("BLE: ai_timer frame too short (%u)\n", (unsigned int)len);
      return false;
    }

  title_len = data[8];
  if (title_len > AI_WATCH_REMINDER_TITLE_MAX)
    {
      printf("BLE: ai_timer label too long (%u)\n", title_len);
      return false;
    }

  if (len != (size_t)AI_WATCH_BLE_CMD_HDR_LEN + title_len)
    {
      printf("BLE: ai_timer length mismatch\n");
      return false;
    }

  if (data[2] == 0)
    {
      printf("BLE: ai_timer id 0 reserved\n");
      return false;
    }

  memcpy(&duration, &data[4], sizeof(duration));
  if (duration < AI_WATCH_AI_TIMER_MIN_S ||
      duration > AI_WATCH_AI_TIMER_MAX_S)
    {
      printf("BLE: ai_timer duration out of range (%lu)\n",
             (unsigned long)duration);
      return false;
    }

  req->id = data[2];
  req->duration_s = duration;
  memcpy(req->label, &data[9], title_len);
  req->label[title_len] = '\0';
  req->pending = true;

  printf("BLE: ai_timer[%u] %lu s \"%s\"\n", req->id,
         (unsigned long)duration, req->label);
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
        return ai_ble_cmd_reminder(data[1], data, len);

      case AI_WATCH_BLE_CMD_AI_TEXT:
        return ai_ble_cmd_ai_text(data, len);

      case AI_WATCH_BLE_CMD_AI_TIMER:
        return ai_ble_cmd_ai_timer(data, len);

      case AI_WATCH_BLE_CMD_CLEAR:
        memset(&g_reminders, 0, sizeof(g_reminders));
        g_alert_head = 0;
        g_alert_count = 0;
        g_ai_timer_req.pending = false;
        g_reminders.pending = true;
        printf("BLE: reminders cleared\n");
        return true;

      default:
        printf("BLE: unknown command 0x%02x\n", data[1]);
        return false;
    }
}

/****************************************************************************
 * Private Functions - Due-time alerts
 *
 * Reminders carry a future UTC trigger time; when it passes, the same
 * alert banner used for arrivals is raised - once per timestamp (the
 * phone re-sending the same id re-arms it). Runs on the main loop;
 * needs a synced RTC (g_last_sync_utc != 0) so an unsynced clock
 * cannot fire the whole store at once.
 ****************************************************************************/

static void ai_ble_check_due(void)
{
  uint32_t now;
  int i;

  if (g_last_sync_utc == 0)
    {
      return;
    }

  now = (uint32_t)time(NULL);

  for (i = 0; i < AI_WATCH_REMINDER_MAX; i++)
    {
      FAR struct ai_watch_reminder_s *item = &g_reminders.items[i];

      if (item->id == 0 ||
          item->type != AI_WATCH_BLE_CMD_REMINDER ||
          item->timestamp == 0 ||
          item->due_alerted)
        {
          continue;
        }

      if ((int32_t)(now - item->timestamp) >= 0)
        {
          item->due_alerted = true;
          ai_ble_alert_push(AI_WATCH_BLE_CMD_REMINDER, item->id,
                            item->timestamp, item->title);
          printf("BLE: reminder[%u] due \"%s\"\n",
                 item->id, item->title);
        }
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
  pthread_attr_t attr;

  memset(&g_reminders, 0, sizeof(g_reminders));
  memset(g_alerts, 0, sizeof(g_alerts));
  g_alert_head = 0;
  g_alert_count = 0;
  g_tz_offset_min = 0;
  g_last_sync_utc = 0;
  memset(&g_ai_result, 0, sizeof(g_ai_result));
  memset(&g_ai_timer_req, 0, sizeof(g_ai_timer_req));

  /* Start the asynchronous DataUpload sender before the bridge: the
   * motion service begins producing frames as soon as the sensor is up.
   */

  if (!g_tx_thread_ok)
    {
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 4096);

      if (pthread_create(&g_tx_thread, &attr, ai_ble_tx_worker,
                         NULL) == 0)
        {
          g_tx_thread_ok = true;
        }
      else
        {
          printf("BLE: tx worker create failed, sync sends only\n");
        }

      pthread_attr_destroy(&attr);
    }

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

  /* Raise banners for reminders whose trigger time just passed */

  ai_ble_check_due();

  /* Controller self-heal: if the LCPU bluetooth firmware reported a
   * hardware error, power-cycle it and rebuild the host stack. */

  if (ai_watch_ble_bsp_take_hw_error())
    {
      ai_watch_ble_bsp_recover();
      return;
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
 * Name: ai_watch_reminder_set_read / _delete / reminders_clear
 *
 * Watch-side reminder lifecycle (see ai_watch_ble.h). Main-loop thread
 * only, like every other user of the store.
 ****************************************************************************/

void ai_watch_reminder_set_read(uint8_t slot, bool read)
{
  if (slot >= AI_WATCH_REMINDER_MAX || g_reminders.items[slot].id == 0)
    {
      return;
    }

  if (read)
    {
      g_reminders.items[slot].flags |= AI_WATCH_REMINDER_FLAG_READ;
    }
  else
    {
      g_reminders.items[slot].flags &= ~AI_WATCH_REMINDER_FLAG_READ;
    }

  g_reminders.pending = true;
}

bool ai_watch_reminder_delete(uint8_t slot)
{
  if (slot >= AI_WATCH_REMINDER_MAX || g_reminders.items[slot].id == 0)
    {
      return false;
    }

  memset(&g_reminders.items[slot], 0,
         sizeof(g_reminders.items[slot]));
  ai_ble_recount();
  g_reminders.pending = true;
  return true;
}

void ai_watch_reminders_clear(void)
{
  memset(&g_reminders, 0, sizeof(g_reminders));
  g_reminders.pending = true;
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

/****************************************************************************
 * Name: ai_watch_ble_post
 *
 * Description:
 *   Asynchronous variant of ai_watch_ble_send_sensor_data: enqueue the
 *   frame for the TX worker thread (started by ai_watch_ble_init) and
 *   return immediately. Use this for everything periodic - steps,
 *   activity, motion-capture and fall reports - never for the AI
 *   trigger, whose send result the UI reports synchronously.
 *
 *   The frame is dropped when the queue is full (radio-paced drain is
 *   slower than peak production while recording) or when BLE is not
 *   connected; both are counted silently by design.
 *
 * Returned Value:
 *   0 when enqueued, negative errno otherwise (-ENOTCONN / -ENOSPC /
 *   -EINVAL, or the direct-send result in the no-worker fallback).
 *
 ****************************************************************************/

int ai_watch_ble_post(uint8_t sensor_type,
                      FAR const void *data,
                      uint8_t data_len)
{
  uint8_t next;

  if (data == NULL || data_len == 0 ||
      data_len > AI_WATCH_BLE_TXQ_DATA_MAX)
    {
      return -EINVAL;
    }

  if (!g_tx_thread_ok)
    {
      /* Worker never started: degrade to the synchronous path */

      return ai_watch_ble_send_sensor_data(sensor_type, data, data_len);
    }

  if (ai_watch_ble_get_state() != AI_WATCH_BLE_BSP_CONNECTED)
    {
      return -ENOTCONN;
    }

  next = (g_txq_head + 1u) % AI_WATCH_BLE_TXQ_LEN;
  if (next == g_txq_tail)
    {
      g_txq_dropped++;
      if ((g_txq_dropped % 100u) == 1u)
        {
          printf("BLE: tx queue full, dropped=%lu\n",
                 (unsigned long)g_txq_dropped);
        }

      return -ENOSPC;
    }

  g_txq[g_txq_head].sensor_type = sensor_type;
  g_txq[g_txq_head].len = data_len;
  memcpy(g_txq[g_txq_head].data, data, data_len);
  g_txq_head = next;

  return 0;
}

/****************************************************************************
 * Name: ai_watch_ble_send_ai_trigger
 ****************************************************************************/

int ai_watch_ble_send_ai_trigger(uint8_t source,
                                 FAR const void *context,
                                 uint8_t context_len)
{
  uint8_t buf[AI_WATCH_BLE_DATAUPLOAD_HDR + 16];
  uint8_t payload_len;

  if (context == NULL && context_len != 0)
    {
      return -EINVAL;
    }

  if ((size_t)context_len + 1 + AI_WATCH_BLE_DATAUPLOAD_HDR >
      sizeof(buf))
    {
      return -EINVAL;
    }

  buf[0] = AI_WATCH_BLE_PROTOCOL_VERSION;
  buf[1] = AI_WATCH_BLE_SENSOR_AI_TRIGGER;
  buf[2] = source;
  if (context_len != 0)
    {
      memcpy(&buf[AI_WATCH_BLE_DATAUPLOAD_HDR + 1], context, context_len);
    }

  payload_len = (uint8_t)(1 + context_len);

  printf("BLE: ai_trigger src=%u ctx=%u\n", source, context_len);

  return ai_watch_ble_bsp_notify(AI_WATCH_BLE_CHR_DATAUPLOAD, buf,
                                 (uint16_t)(AI_WATCH_BLE_DATAUPLOAD_HDR +
                                            payload_len));
}

/****************************************************************************
 * Name: ai_watch_ble_get_ai_result
 ****************************************************************************/

FAR struct ai_watch_ai_result_s *ai_watch_ble_get_ai_result(void)
{
  return &g_ai_result;
}

/****************************************************************************
 * Name: ai_watch_ble_take_ai_timer
 ****************************************************************************/

bool ai_watch_ble_take_ai_timer(FAR struct ai_watch_ai_timer_req_s *req)
{
  if (req == NULL || !g_ai_timer_req.pending)
    {
      return false;
    }

  memcpy(req, &g_ai_timer_req, sizeof(*req));
  g_ai_timer_req.pending = false;
  return true;
}
