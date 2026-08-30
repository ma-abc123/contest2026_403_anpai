/****************************************************************************
 * apps/ai_watch/ai_watch_ble_bsp.h
 *
 * Stable plain-C wrapper between the AI Watch application and the
 * SiFli/zblue BLE bridge.
 *
 *   Implementation: vendor/sifli/chips/sf32lb52/sf32lb52_ble_bridge.c
 *                   (the only TU outside apps/external/zblue that may
 *                    include zblue headers)
 *   Consumer:       app/ai_watch/ai_watch_ble.c
 *
 * The application must not include zblue/Zephyr headers anywhere. All
 * cross-thread data exchange is performed inside the bridge: callbacks
 * invoked by the zblue RX thread only enqueue data, and the application
 * drains it from its main (LVGL) thread through the take_* functions
 * below. No LVGL API may be called from bridge callbacks.
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

#ifndef __APPS_AI_WATCH_AI_WATCH_BLE_BSP_H
#define __APPS_AI_WATCH_AI_WATCH_BLE_BSP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Character slot indexes inside the custom service. The full 128-bit
 * UUIDs (little-endian on air) are documented in ai_watch_ble.h.
 */

#define AI_WATCH_BLE_CHR_STATUS     0   /* f1: Read + Notify */
#define AI_WATCH_BLE_CHR_TIMESYNC   1   /* f2: Write (consumed by bridge) */
#define AI_WATCH_BLE_CHR_DATAUPLOAD 2   /* f3: Notify */
#define AI_WATCH_BLE_CHR_COMMAND    3   /* f4: Write (queued to app) */

/* TimeSync payload: version(1) + utc(4) + tz_offset_minutes(2), LE */

#define AI_WATCH_BLE_TIMESYNC_SIZE  7

/* Command frame queue: one frame is at most
 * version(1)+cmd(1)+id(1)+flags(1)+ts(4)+len(1)+title(24) = 33 bytes
 */

#define AI_WATCH_BLE_CMD_MAX        64
#define AI_WATCH_BLE_CMD_QUEUE_LEN  4

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Bridge lifecycle / link state, polled by the application */

enum ai_watch_ble_bsp_state_e
{
  AI_WATCH_BLE_BSP_OFF = 0,         /* start() not called yet */
  AI_WATCH_BLE_BSP_INIT_FAILED,     /* bt_enable or HCI open failed */
  AI_WATCH_BLE_BSP_ADVERTISING,     /* connectable advertising */
  AI_WATCH_BLE_BSP_CONNECTED,       /* central connected */
  AI_WATCH_BLE_BSP_DISCONNECTED,    /* was connected, link lost */
};

/* Validated TimeSync payload (little-endian fields as received) */

struct ai_watch_ble_bsp_timesync_s
{
  uint8_t version;                  /* protocol version, must be 1 */
  uint32_t utc_seconds;             /* UTC Unix seconds */
  int16_t tz_offset_min;            /* minutes east of UTC */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ai_watch_ble_bsp_start
 *
 * Description:
 *   Bring up the BLE host: bt_enable(), register the AI Watch custom GATT
 *   service, then start connectable advertising with the device name from
 *   CONFIG_BT_DEVICE_NAME. Safe to call once from the application main
 *   thread; completion is asynchronous - poll ai_watch_ble_bsp_get_state().
 *
 *   The HCI transport (/dev/ttyHCI0, LCPU controller) must already be
 *   registered by the board (CONFIG_UART_BTH4).
 *
 * Returned Value:
 *   0 if the bring-up was kicked off, negative errno otherwise.
 *
 ****************************************************************************/

int ai_watch_ble_bsp_start(void);

/****************************************************************************
 * Name: ai_watch_ble_bsp_get_state
 *
 * Description:
 *   Current bridge state. Callable from any thread.
 *
 ****************************************************************************/

enum ai_watch_ble_bsp_state_e ai_watch_ble_bsp_get_state(void);

/****************************************************************************
 * Name: ai_watch_ble_bsp_take_timesync
 *
 * Description:
 *   Consume one pending, already validated TimeSync write. The payload is
 *   zeroed by the bridge until a phone writes it; returns false when no
 *   sync is pending. Callable only from the application thread.
 *
 ****************************************************************************/

bool ai_watch_ble_bsp_take_timesync(
    FAR struct ai_watch_ble_bsp_timesync_s *out);

/****************************************************************************
 * Name: ai_watch_ble_bsp_take_command
 *
 * Description:
 *   Dequeue one raw Command characteristic write (version, cmd_type, ...)
 *   received from the phone. Copies at most buflen bytes; returns the
 *   number of bytes copied, or 0 if the queue is empty. Callable only
 *   from the application thread; parsing and validation happen there.
 *
 ****************************************************************************/

size_t ai_watch_ble_bsp_take_command(FAR uint8_t *buf, size_t buflen);

/****************************************************************************
 * Name: ai_watch_ble_bsp_notify
 *
 * Description:
 *   Send a notification on one of the service's notify characteristics.
 *   Fails with -ENOTCONN when no central is connected and -EACCES when
 *   the peer has not subscribed to that characteristic's CCC.
 *
 *   chr: AI_WATCH_BLE_CHR_STATUS or AI_WATCH_BLE_CHR_DATAUPLOAD
 *
 ****************************************************************************/

int ai_watch_ble_bsp_notify(uint8_t chr, FAR const void *data,
                            uint16_t len);

/****************************************************************************
 * Name: ai_watch_ble_bsp_resume_advertising
 *
 * Description:
 *   Re-enter connectable advertising after a link loss (state must be
 *   DISCONNECTED). Called from the application main loop, which should
 *   retry roughly once per second until the state flips back to
 *   ADVERTISING. No-op when Bluetooth is disabled via
 *   ai_watch_ble_bsp_set_enabled(false).
 *
 ****************************************************************************/

void ai_watch_ble_bsp_resume_advertising(void);

/****************************************************************************
 * Name: ai_watch_ble_bsp_take_hw_error / ai_watch_ble_bsp_recover
 *
 * Description:
 *   Controller self-heal.  When the LCPU bluetooth firmware dies it
 *   reports a Hardware Error event; the bridge latches that as a
 *   pending flag (RX-thread context).  Poll take_hw_error() from the
 *   main loop and call recover() when it returns true: the LCPU is
 *   power-cycled (patches + RF calibration re-run), the zblue host is
 *   torn down and rebuilt, and advertising resumes.
 *
 ****************************************************************************/

bool ai_watch_ble_bsp_take_hw_error(void);

void ai_watch_ble_bsp_recover(void);

/****************************************************************************
 * Name: ai_watch_ble_bsp_set_enabled
 *
 * Description:
 *   Application-visible Bluetooth switch (defaults to enabled).
 *   Disabling stops advertising and terminates any active connection;
 *   the state becomes OFF and stays there until re-enabled. Enabling
 *   restores advertising (performs the full host bring-up first if
 *   bsp_start() has never run). Must be called from the application
 *   thread.
 *
 ****************************************************************************/

void ai_watch_ble_bsp_set_enabled(bool enabled);

#endif /* __APPS_AI_WATCH_AI_WATCH_BLE_BSP_H */
