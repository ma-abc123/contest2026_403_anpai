/****************************************************************************
 * apps/ai_watch/ai_watch_motion.h
 *
 * Background motion service on the onboard LSM6DS3TR-C (/dev/lsm6dsl0).
 *
 * A pthread owns the char device: open -> SNIOC_START -> poll loop with
 * all DSP inside the thread. The LVGL main loop only touches the shared
 * state through ai_watch_motion_process(), the same pattern that keeps
 * the heart-rate page off the I2C driver lock.
 *
 * Raw test-set recording (M4 acceptance material) is also collected
 * here: ai_watch_motion_record_start() switches the thread into capture
 * mode; the lines are printed and forwarded over BLE by
 * ai_watch_motion_process() on the main loop, so the UART and the
 * radio are never written from the sampling thread.
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
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#ifndef __APPS_AI_WATCH_AI_WATCH_MOTION_H
#define __APPS_AI_WATCH_AI_WATCH_MOTION_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Activity classification */

#define AI_WATCH_MOTION_REST    0
#define AI_WATCH_MOTION_WALK    1
#define AI_WATCH_MOTION_RUN     2

/* Fall event kinds (BLE FALL_EVENT payload byte 0 and UI) */

#define AI_WATCH_FALL_NONE      0  /* no event */
#define AI_WATCH_FALL_CONFIRMED 1  /* countdown expired uncancelled */
#define AI_WATCH_FALL_CANCELLED 2  /* user cancelled the countdown */
#define AI_WATCH_FALL_TEST      3  /* manual test trigger */

/* Time the "Suspected fall" countdown runs before reporting (ms) */

#define AI_WATCH_FALL_COUNTDOWN_MS      15000

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Fall state machine (NORMAL -> IMPACT -> ALARM -> NORMAL) */

enum ai_watch_fall_state_e
{
  FALL_STATE_NORMAL = 0,   /* monitoring */
  FALL_STATE_IMPACT,       /* spike seen, watching for stillness */
  FALL_STATE_ALARM         /* still + orientation flip: countdown to
                            * the "suspected fall" report */
};

/* Snapshot published for the Exercise UI (mutex-guarded, and mutated
 * only by the sampling thread - except fall_pending / fall_deadline_ms,
 * which the cancel/test helpers also touch, always under the mutex).
 */

struct ai_watch_motion_snapshot_s
{
  uint32_t steps_today;         /* peak-detected step count */
  uint32_t distance_m;          /* steps * stride estimate */
  uint8_t  activity;            /* AI_WATCH_MOTION_* (debounced) */
  uint8_t  activity_confident;  /* bool: enough samples classified */
  uint16_t activity_std_mg;     /* current 2 s window std dev */
  bool     sensor_ok;           /* IMU opened and streaming */
  uint32_t last_mag_mg;         /* last |a| sample in mg (live bar) */
  bool     recording;           /* CSV test-set recording active */
  uint32_t record_lines;        /* samples captured since record start */

  /* Fall pipeline state */

  enum ai_watch_fall_state_e fall_state;
  uint8_t  fall_pending;        /* 1 while the countdown is cancellable */
  uint32_t fall_deadline_ms;    /* CLOCK_MONOTONIC ms of expiry */
  int16_t  fall_impact_mg;      /* peak |a| of the detected impact */
  uint8_t  fall_angle_deg;      /* orientation change estimate */
  bool     fall_test;           /* countdown started by the test button;
                                 * its outcome reports as FALL_TEST */
};

/* One fall report (UI + BLE payload without the reserved tail) */

struct ai_watch_fall_report_s
{
  uint8_t  event;               /* AI_WATCH_FALL_* */
  uint16_t impact_mg;
  uint8_t  angle_deg;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ai_watch_motion_init
 *
 * Description:
 *   Reset the shared state and spawn the sampling thread. Call once from
 *   the app main() before entering the LVGL loop; runs until process
 *   exit.
 *
 ****************************************************************************/

void ai_watch_motion_init(void);

/****************************************************************************
 * Name: ai_watch_motion_process
 *
 * Description:
 *   Main-loop drain: prints + BLE-forwards captured recording samples,
 *   and paces the steps/activity DataUpload reports. Call every main
 *   loop iteration.
 *
 ****************************************************************************/

void ai_watch_motion_process(void);

/****************************************************************************
 * Name: ai_watch_motion_get_snapshot
 *
 * Description:
 *   Copy the current snapshot under the mutex. Safe from any thread.
 *
 ****************************************************************************/

void ai_watch_motion_get_snapshot(
    FAR struct ai_watch_motion_snapshot_s *out);

/****************************************************************************
 * Name: ai_watch_motion_record_start / _stop
 *
 * Description:
 *   Test-set recording (M4 step 2). While active every raw sample is
 *   queued to the main loop, which prints it as CSV:
 *     t_ms,x_mg,y_mg,z_mg,gx_mdps,gy_mdps,gz_mdps
 *   and forwards a decimated copy over BLE (sensor_type 0x12,
 *   MOTION_DATA). _start fails if the sensor is down or a recording is
 *   already running.
 *
 ****************************************************************************/

bool ai_watch_motion_record_start(void);

void ai_watch_motion_record_stop(void);

/****************************************************************************
 * Name: ai_watch_motion_test_trigger
 *
 * Description:
 *   Jump the fall pipeline straight into the countdown state so the
 *   full alarm/report path can be exercised without dropping the watch.
 *   Fails when a fall countdown is already pending.
 *
 ****************************************************************************/

bool ai_watch_motion_test_trigger(void);

/****************************************************************************
 * Name: ai_watch_motion_fall_cancel
 *
 * Description:
 *   Cancel a pending "suspected fall" countdown (alarm UI button or
 *   KEY2). Emits a CANCELLED event consumable via
 *   ai_watch_motion_poll_fall(). Returns false when nothing was pending.
 *
 ****************************************************************************/

bool ai_watch_motion_fall_cancel(void);

/****************************************************************************
 * Name: ai_watch_motion_poll_fall
 *
 * Description:
 *   Pop the oldest finished fall event (countdown expired uncancelled,
 *   cancelled by the user, or a test run completed), send its FALL_EVENT
 *   report over BLE and fill *out for the UI. Returns false when no
 *   event is pending. Main loop only.
 *
 ****************************************************************************/

bool ai_watch_motion_poll_fall(FAR struct ai_watch_fall_report_s *out);

#endif /* __APPS_AI_WATCH_AI_WATCH_MOTION_H */
