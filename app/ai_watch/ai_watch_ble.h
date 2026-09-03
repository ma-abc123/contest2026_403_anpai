/****************************************************************************
 * apps/ai_watch/ai_watch_ble.h
 *
 * Application-side BLE protocol definitions and API.
 *
 * All zblue interaction lives behind the stable wrapper declared in
 * ai_watch_ble_bsp.h (implemented in the BSP: sf32lb52_ble_bridge.c).
 * This module owns the AI Watch protocol: TimeSync handling, the Command
 * frame parser and the reminder store. Every function here is safe to
 * call from the LVGL main loop; all BLE callback data is drained there,
 * so no locking is required and LVGL is never touched from BLE context.
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

#ifndef __APPS_AI_WATCH_AI_WATCH_BLE_H
#define __APPS_AI_WATCH_AI_WATCH_BLE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "ai_watch_ble_bsp.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Wire protocol version - bump when a payload format changes */

#define AI_WATCH_BLE_PROTOCOL_VERSION   1

/* Advertised device name (mirrors CONFIG_BT_DEVICE_NAME in the defconfig;
 * the air name comes from the bridge advertising data).
 */

#define AI_WATCH_BLE_DEVICE_NAME        "AI-Watch-403"

/* Custom GATT service / characteristic UUIDs, as they appear to a phone.
 * On-air (ATT) these 128-bit values are little-endian; the bridge uses
 * BT_UUID_128_ENCODE() which encodes them correctly.
 *
 *   Service    12345678-9abc-def0-1234-56789abcdef0
 *   Status  f1 12345678-9abc-def0-1234-56789abcdef1  Read + Notify
 *   TimeSync f2 12345678-9abc-def0-1234-56789abcdef2  Write
 *   DataUp  f3 12345678-9abc-def0-1234-56789abcdef3  Notify
 *   Command f4 12345678-9abc-def0-1234-56789abcdef4  Write
 */

#define AI_WATCH_BLE_SVC_UUID_STR       \
  "12345678-9abc-def0-1234-56789abcdef0"
#define AI_WATCH_BLE_STATUS_UUID_STR    \
  "12345678-9abc-def0-1234-56789abcdef1"
#define AI_WATCH_BLE_TIMESYNC_UUID_STR  \
  "12345678-9abc-def0-1234-56789abcdef2"
#define AI_WATCH_BLE_DATAUPLOAD_UUID_STR \
  "12345678-9abc-def0-1234-56789abcdef3"
#define AI_WATCH_BLE_COMMAND_UUID_STR   \
  "12345678-9abc-def0-1234-56789abcdef4"

/* TimeSync payload: version(1) + utc(4) + tz_offset_minutes(2), LE */

#define AI_WATCH_BLE_TIMESYNC_SIZE      7

/* Status payload: version(1) + conn_state(1) + last_sync(4), LE */

#define AI_WATCH_BLE_STATUS_SIZE        6

/* DataUpload payload header: version(1) + sensor_type(1) */

#define AI_WATCH_BLE_DATAUPLOAD_HDR     2

/* DataUpload sensor types */

#define AI_WATCH_BLE_SENSOR_STEPS       0x05  /* uint32 LE, steps today */
#define AI_WATCH_BLE_SENSOR_ACTIVITY    0x06  /* uint8, AI_WATCH_MOTION_*
                                               * (0 rest / 1 walk / 2 run) */
#define AI_WATCH_BLE_SENSOR_AI_TRIGGER  0x10  /* AI voice trigger, see
                                               * ai_watch_ble_send_ai_trigger */
#define AI_WATCH_BLE_SENSOR_FALL_EVENT  0x11  /* suspected-fall report:
                                               * event(1) + impact_mg(2,LE)
                                               * + angle_deg(1) + rsvd(4) */
#define AI_WATCH_BLE_SENSOR_MOTION_DATA 0x12  /* raw capture (recording):
                                               * t_ms(4, LE) + x/y/z(6) +
                                               * gx/gy/gz(6), mg/mdps,
                                               * decimated view */

/* AI trigger payload: source(1) [+ optional context bytes] */

#define AI_WATCH_AI_TRIGGER_SRC_PAGE    0x01  /* AI app page button */

/* Command frame: [version(1)][cmd_type(1)][id(1)][flags(1)]
 *                [timestamp(4)][title_len(1)][title(N)]      (all LE)
 */

#define AI_WATCH_BLE_CMD_REMINDER       0x01
#define AI_WATCH_BLE_CMD_NOTIFICATION   0x02
#define AI_WATCH_BLE_CMD_CLEAR          0x03
#define AI_WATCH_BLE_CMD_AI_TEXT        0x04  /* AI result text in title */
#define AI_WATCH_BLE_CMD_AI_TIMER       0x05  /* countdown; timestamp field
                                               * carries duration seconds */

#define AI_WATCH_BLE_CMD_HDR_LEN        9   /* through title_len */

/* AI result text limit: the bridge queue takes frames up to
 * AI_WATCH_BLE_CMD_MAX (240), header is 9 bytes.
 */

#define AI_WATCH_AI_TEXT_MAX            224

/* AI countdown duration limits (seconds) */

#define AI_WATCH_AI_TIMER_MIN_S         1
#define AI_WATCH_AI_TIMER_MAX_S         86400  /* 24 h */

/* Reminder flag bits */

#define AI_WATCH_REMINDER_FLAG_READ     0x01
#define AI_WATCH_REMINDER_FLAG_ACTIVE   0x02

/* Reminder store limits */

#define AI_WATCH_REMINDER_MAX           8
#define AI_WATCH_REMINDER_TITLE_MAX     24

/* Pending-alert ring depth: how many arrivals can wait for the banner */

#define AI_WATCH_BLE_ALERT_RING_LEN     4

/* TimeSync validity window (matches bridge validation) */

#define AI_WATCH_TS_MIN_UTC             1577836800u   /* 2020-01-01 */
#define AI_WATCH_TS_MAX_UTC             4102444800u   /* 2100-01-01 */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One reminder received over the Command characteristic */

struct ai_watch_reminder_s
{
  uint8_t id;                   /* Unique ID (0 = empty slot) */
  uint8_t flags;                /* AI_WATCH_REMINDER_FLAG_* */
  uint8_t type;                 /* AI_WATCH_BLE_CMD_REMINDER or _NOTIFICATION */
  uint32_t timestamp;           /* UTC Unix seconds */
  char title[AI_WATCH_REMINDER_TITLE_MAX + 1];
  bool due_alerted;             /* due-time banner already shown (watch-
                                 * local; reset when the phone re-sends
                                 * this id) */
};

/* Reminder store - owned by ai_watch_ble.c, mutated only from the main
 * loop (draining Command frames), read by the reminder UI.
 */

struct ai_watch_reminder_store_s
{
  struct ai_watch_reminder_s items[AI_WATCH_REMINDER_MAX];
  uint8_t count;
  bool pending;                 /* set after any change, for UI refresh */
};

/* One arrival waiting to be shown as the on-screen alert banner */

struct ai_watch_ble_alert_s
{
  uint8_t type;                 /* AI_WATCH_BLE_CMD_* (also used for the
                                 * AI countdown expiry, _AI_TIMER) */
  uint8_t id;
  uint32_t timestamp;           /* UTC Unix seconds */
  char title[AI_WATCH_REMINDER_TITLE_MAX + 1];
};

/* Last AI result text (AI_TEXT command). Owned by ai_watch_ble.c,
 * mutated only from the main loop; polled by the AI page UI.
 */

struct ai_watch_ai_result_s
{
  char text[AI_WATCH_AI_TEXT_MAX + 1];
  uint32_t timestamp;           /* UTC Unix seconds from the frame */
  uint8_t id;                   /* request id from the frame */
  bool pending;                 /* new text not rendered yet */
};

/* One pending AI countdown request (AI_TIMER command) */

struct ai_watch_ai_timer_req_s
{
  uint8_t id;
  uint32_t duration_s;          /* 1..AI_WATCH_AI_TIMER_MAX_S */
  char label[AI_WATCH_REMINDER_TITLE_MAX + 1];
  bool pending;                 /* set by the parser, cleared by the reader */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ai_watch_ble_init
 *
 * Description:
 *   Kick off BLE bring-up (bridge: bt_enable, GATT service, advertising).
 *   Completion is asynchronous; poll ai_watch_ble_get_state().
 *
 * Returned Value:
 *   0 on success, negative errno on failure.
 *
 ****************************************************************************/

int ai_watch_ble_init(void);

/****************************************************************************
 * Name: ai_watch_ble_get_state / ai_watch_ble_get_status_text
 ****************************************************************************/

enum ai_watch_ble_bsp_state_e ai_watch_ble_get_state(void);

FAR const char *ai_watch_ble_get_status_text(
    enum ai_watch_ble_bsp_state_e state);

/****************************************************************************
 * Name: ai_watch_ble_process
 *
 * Description:
 *   Drain all pending BLE events. Must be called from the main loop:
 *   - consumes a validated TimeSync write: sets the RTC to UTC, stores
 *     the timezone offset for display-only use and notifies Status
 *   - consumes Command frames: updates the reminder store
 *
 ****************************************************************************/

void ai_watch_ble_process(void);

/****************************************************************************
 * Name: ai_watch_ble_set_enabled
 *
 * Description:
 *   Turn the Bluetooth radio on/off (the Settings switch). Off: stop
 *   advertising, drop any connection, home label shows "BLE: OFF".
 *   On: resume advertising (full bring-up if needed). Must be called
 *   from the main loop thread.
 *
 ****************************************************************************/

void ai_watch_ble_set_enabled(bool enabled);

/****************************************************************************
 * Name: ai_watch_ble_get_tz_offset_min / ai_watch_ble_get_last_sync
 *
 * Description:
 *   Timezone offset (minutes east of UTC) received with the last
 *   successful TimeSync (0 before the first sync), and the UTC time of
 *   that sync (0 = never).
 *
 ****************************************************************************/

int16_t ai_watch_ble_get_tz_offset_min(void);

uint32_t ai_watch_ble_get_last_sync(void);

/****************************************************************************
 * Name: ai_watch_ble_localtime
 *
 * Description:
 *   Convert a UTC time to watch-local time using the synced timezone
 *   offset. The RTC itself always stores UTC; the offset is applied only
 *   here, for display.
 *
 ****************************************************************************/

FAR struct tm *ai_watch_ble_localtime(time_t utc, FAR struct tm *tm);

/****************************************************************************
 * Name: ai_watch_ble_get_reminders
 *
 * Description:
 *   Reminder store pointer for the reminder UI.
 *
 ****************************************************************************/

FAR struct ai_watch_reminder_store_s *ai_watch_ble_get_reminders(void);

/****************************************************************************
 * Name: ai_watch_ble_take_alert
 *
 * Description:
 *   Pop the oldest arrival (reminder or notification) that has not been
 *   shown as an alert banner yet. Call repeatedly from the main loop
 *   until it returns false. Command frames parsed in ai_watch_ble_process()
 *   enqueue here, so this is safe to call from the LVGL thread.
 *
 * Returned Value:
 *   true and fills *out when an alert is pending; false otherwise.
 *
 ****************************************************************************/

bool ai_watch_ble_take_alert(FAR struct ai_watch_ble_alert_s *out);

/****************************************************************************
 * Name: ai_watch_reminder_set_read / _delete / reminders_clear
 *
 * Description:
 *   Watch-side reminder lifecycle, called from the Reminder UI (main-loop
 *   thread, same thread as the command parser - no locking):
 *   - set_read:  toggle the AI_WATCH_REMINDER_FLAG_READ bit of one slot
 *   - delete:    remove one reminder (id 0 = empty slot), compacting is
 *                not needed - the UI hides empty slots
 *   - clear:     drop every reminder (same as the phone CLEAR command)
 *
 *   All three mark the store pending so the Reminder UI re-renders and
 *   the home-page unread count refreshes.
 *
 * Input Parameters:
 *   slot - store index (0..AI_WATCH_REMINDER_MAX-1); must reference a
 *          used slot (id != 0) for set_read/delete, ignored otherwise.
 *
 * Returned Value:
 *   delete returns true when the slot was removed.
 *
 ****************************************************************************/

void ai_watch_reminder_set_read(uint8_t slot, bool read);

bool ai_watch_reminder_delete(uint8_t slot);

void ai_watch_reminders_clear(void);

/****************************************************************************
 * Name: ai_watch_ble_send_sensor_data
 *
 * Description:
 *   Notify one sensor sample on DataUpload (f3):
 *   [version(1)][sensor_type(1)][payload(N)] with payload in little-endian.
 *   sensor_type: AI_WATCH_BLE_SENSOR_*.
 *
 * Returned Value:
 *   0 on success, negative errno (-ENOTCONN / -EACCES) otherwise.
 *
 ****************************************************************************/

int ai_watch_ble_send_sensor_data(uint8_t sensor_type,
                                  FAR const void *data,
                                  uint8_t data_len);

/****************************************************************************
 * Name: ai_watch_ble_post
 *
 * Description:
 *   Asynchronous ai_watch_ble_send_sensor_data: the frame is queued for
 *   the TX worker thread (ai_watch_ble_init starts it) so the LVGL loop
 *   never blocks on the ~1-connection-interval bt_gatt_indicate() call.
 *   Use for ALL periodic uploads (steps, activity, motion capture, fall
 *   reports); keep only the AI trigger synchronous so its button can
 *   report the send result. Drops silently when BLE is down or the
 *   queue is full.
 *
 * Returned Value:
 *   0 when enqueued (or sent directly in the no-worker fallback),
 *   negative errno otherwise.
 *
 ****************************************************************************/

int ai_watch_ble_post(uint8_t sensor_type,
                      FAR const void *data,
                      uint8_t data_len);

/****************************************************************************
 * Name: ai_watch_ble_send_ai_trigger
 *
 * Description:
 *   Tell the phone to start a voice/AI round: notify on DataUpload (f3)
 *   as sensor_type AI_WATCH_BLE_SENSOR_AI_TRIGGER with payload
 *   [source(1)][context(N)]:
 *   - source: AI_WATCH_AI_TRIGGER_SRC_* (page button)
 *   - context: optional bytes for that source; may be 0 bytes.
 *
 *   The phone answers with AI_TEXT / AI_TIMER / reminder commands.
 *
 * Returned Value:
 *   0 on success, negative errno (-ENOTCONN / -EACCES / -EINVAL) otherwise.
 *
 ****************************************************************************/

int ai_watch_ble_send_ai_trigger(uint8_t source,
                                 FAR const void *context,
                                 uint8_t context_len);

/****************************************************************************
 * Name: ai_watch_ble_get_ai_result
 *
 * Description:
 *   Last AI result text received via AI_TEXT (empty string before the
 *   first one). Check .pending to know the text changed since it was
 *   last rendered; the reader clears .pending.
 *
 ****************************************************************************/

FAR struct ai_watch_ai_result_s *ai_watch_ble_get_ai_result(void);

/****************************************************************************
 * Name: ai_watch_ble_take_ai_timer
 *
 * Description:
 *   Consume one pending AI_TIMER request (new countdown to start; a
 *   second request replaces the previous one). Returns false when none
 *   is pending. Callable from the main loop thread.
 *
 ****************************************************************************/

bool ai_watch_ble_take_ai_timer(
    FAR struct ai_watch_ai_timer_req_s *req);

#endif /* __APPS_AI_WATCH_AI_WATCH_BLE_H */
