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

#define AI_WATCH_BLE_SENSOR_TEMP        0x01  /* float32 LE, degC */
#define AI_WATCH_BLE_SENSOR_HUM         0x02  /* float32 LE, %RH */
#define AI_WATCH_BLE_SENSOR_HR          0x03  /* uint16 LE, bpm */
#define AI_WATCH_BLE_SENSOR_SPO2        0x04  /* uint16 LE, % */

/* Command frame: [version(1)][cmd_type(1)][id(1)][flags(1)]
 *                [timestamp(4)][title_len(1)][title(N)]      (all LE)
 */

#define AI_WATCH_BLE_CMD_REMINDER       0x01
#define AI_WATCH_BLE_CMD_NOTIFICATION   0x02
#define AI_WATCH_BLE_CMD_CLEAR          0x03

#define AI_WATCH_BLE_CMD_HDR_LEN        9   /* through title_len */

/* Reminder flag bits */

#define AI_WATCH_REMINDER_FLAG_READ     0x01
#define AI_WATCH_REMINDER_FLAG_ACTIVE   0x02

/* Reminder store limits */

#define AI_WATCH_REMINDER_MAX           8
#define AI_WATCH_REMINDER_TITLE_MAX     24

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
  uint32_t timestamp;           /* UTC Unix seconds */
  char title[AI_WATCH_REMINDER_TITLE_MAX + 1];
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

#endif /* __APPS_AI_WATCH_AI_WATCH_BLE_H */
