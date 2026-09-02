/****************************************************************************
 * apps/ai_watch/ai_watch_motion.c
 *
 * Background motion service on the onboard LSM6DS3TR-C.
 * See ai_watch_motion.h for the architecture notes.
 *
 * Algorithms (initial thresholds; the M4 recorded test sets are the
 * reference for tuning - see tools/fall_threshold_analysis.py):
 *
 * Steps: |a| hysteresis detector (peak above ~1.15 g, valley back under
 *        ~0.85 g) with a 300 ms refractory period.
 *
 * Activity: std dev of |a| over a 2 s window; < 30 mg = rest, > 350 mg
 *        = run, in between = walk. A classification must repeat for
 *        ~1 s before it is published (debounce).
 *
 * Fall (reports always say "suspected fall", never medical grade):
 *        NORMAL -|impact spike|-> IMPACT -|3 s quiet tail + gravity
 *        direction flipped >= 50 deg|-> ALARM: a 15 s cancellable
 *        countdown, then a CONFIRMED event. Cancel and test paths
 *        mutate the shared state from the main loop under the mutex.
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/ioctl.h>

#include <nuttx/sensors/lsm6dsl.h>

#include "ai_watch_ble.h"
#include "ai_watch_motion.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LSM6DSL_DEV_PATH        "/dev/lsm6dsl0"

/* Poll rate. The in-tree test driver has no ODR control: it streams
 * accel+gyro at a fixed 833 Hz internally, so this loop is a decimating
 * reader. Each SNIOC_LSM6DSLSENSORREAD walks ~22 registers byte-wise
 * over I2C (~5-8 ms), which sets the realistic ceiling. A ~30 ms period
 * still brackets 50-200 ms body impacts with several samples while
 * leaving bus time to the heart-rate page.
 */

#define MOTION_PERIOD_MS        25

/* Recording: BLE MOTION_DATA forwarding decimation. The radio path is
 * a sampled view (one frame in BLE_REC_DEC); the serial CSV log always
 * carries every sample.
 */

#define BLE_REC_DEC             4

/* Step detector (mg on |a|, gravity included) */

#define STEP_PEAK_MG            1150
#define STEP_VALLEY_MG          850
#define STEP_REFRACT_MS         300
#define STEP_STRIDE_MM          700

/* Activity classifier (mg std dev over ~2 s of samples) */

#define ACT_REST_STD_MG         30
#define ACT_RUN_STD_MG          350
#define ACT_DEBOUNCE_SAMPLES    40

/* Fall detector (mg unless noted). Impact rule (any of):
 *   - single sample above FALL_IMPACT_HARD_MG
 *   - two consecutive samples above FALL_IMPACT_SOFT_MG
 *   - a free-fall sample (< FALL_FREEFALL_MG) within FF_SETTLE_MS and
 *     then a single sample above FALL_IMPACT_SOFT_MG
 * Walking peaks stay ~400 mg below the soft level and never reach
 * free-fall, so both gates must agree before a real fall.
 */

#define FALL_FREEFALL_MG        550     /* |a| below: free-fall latched */
#define FALL_IMPACT_HARD_MG     2400    /* single-sample spike */
#define FALL_IMPACT_SOFT_MG     2000    /* FF-gated / two consecutive */
#define FALL_STILL_WINDOW_MS    3000    /* post-impact observation */
#define FALL_STILL_DEV_MG       80      /* max |a| deviation while still */
#define FALL_JOLT_MG            200     /* deviation that re-arms the window */
#define FALL_ANGLE_MIN_DEG      50      /* gravity direction change */
#define FALL_LONG_STILL_MS      4500    /* flat-drop path: still this long
                                         * after a free-fall impact */
#define FALL_IMPACT_MAX_OBS_MS  10000   /* observation budget before
                                         * rejecting an impact */
#define FALL_COOLDOWN_MS        60000   /* re-arm after a finished event */
#define FALL_REJECT_COOLDOWN_MS 8000    /* shorter blind window after a
                                         * rejected impact, so slam/pickup
                                         * test patterns are not swallowed */

/* Shared queues between the sampling thread and the main loop */

#define MOTION_FALL_QLEN        4
#define MOTION_REC_QLEN         96

/* Filter windows in samples (~30 ms each): dev ring 1 s, activity ring
 * 2 s, free-fall settle 0.5 s.
 */

#define DEV_RING_LEN            34
#define DEV_RING_MS             1000
#define ACT_RING_LEN            68
#define FF_SETTLE_MS            500

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct motion_rec_sample_s
{
  uint32_t t_ms;
  int16_t x;
  int16_t y;
  int16_t z;
  int16_t gx;
  int16_t gy;
  int16_t gz;
};

struct motion_state_s
{
  pthread_mutex_t lock;
  struct ai_watch_motion_snapshot_s snap;

  /* Fall results: produced by the sampling thread (and by the cancel
   * helper under the same mutex), consumed by the main loop only.
   */

  struct ai_watch_fall_report_s fall_q[MOTION_FALL_QLEN];
  volatile uint8_t fall_q_head;
  volatile uint8_t fall_q_tail;

  /* Recording samples: produced by the thread, consumed on the main
   * loop (UART CSV + decimated BLE frames).
   */

  struct motion_rec_sample_s rec_q[MOTION_REC_QLEN];
  volatile uint8_t rec_q_head;
  volatile uint8_t rec_q_tail;

  pthread_t thread;
  volatile bool thread_running;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct motion_state_s g_motion;

/* BLE upload pacing (main loop only) */

static uint32_t g_steps_sent;
static uint32_t g_steps_sent_ms;
static uint8_t g_activity_sent;
static uint32_t g_activity_sent_ms;
static unsigned g_rec_ble_seq;
static bool g_ble_first_pass = true;

/****************************************************************************
 * Private Functions - Helpers
 ****************************************************************************/

static uint32_t motion_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* Push a fall result; callers hold g_motion.lock (or are the only
 * producer at that moment). Drops the oldest-overflow case silently.
 */

static void motion_fall_push_locked(uint8_t event, int16_t impact_mg,
                                    uint8_t angle_deg)
{
  uint8_t next;

  next = (g_motion.fall_q_head + 1u) % MOTION_FALL_QLEN;
  if (next == g_motion.fall_q_tail)
    {
      return;
    }

  g_motion.fall_q[g_motion.fall_q_head].event = event;
  g_motion.fall_q[g_motion.fall_q_head].impact_mg = impact_mg;
  g_motion.fall_q[g_motion.fall_q_head].angle_deg = angle_deg;
  g_motion.fall_q_head = next;
}

static void motion_rec_push(FAR const struct motion_rec_sample_s *s)
{
  uint8_t next;

  next = (g_motion.rec_q_head + 1u) % MOTION_REC_QLEN;
  if (next == g_motion.rec_q_tail)
    {
      return;                     /* queue full: drop (diagnostic path) */
    }

  g_motion.rec_q[g_motion.rec_q_head] = *s;
  g_motion.rec_q_head = next;
}

/* Angle between two gravity direction vectors, in degrees */

static uint16_t motion_angle_deg(float ax, float ay, float az,
                                 float bx, float by, float bz)
{
  float dot;
  float na;
  float nb;
  float c;

  dot = ax * bx + ay * by + az * bz;
  na = sqrtf(ax * ax + ay * ay + az * az);
  nb = sqrtf(bx * bx + by * by + bz * bz);

  if (na < 1.0f || nb < 1.0f)
    {
      return 0;
    }

  c = dot / (na * nb);
  if (c > 1.0f)
    {
      c = 1.0f;
    }
  else if (c < -1.0f)
    {
      c = -1.0f;
    }

  return (uint16_t)(acosf(c) * 57.2958f);
}

/****************************************************************************
 * Private Functions - Sampling thread
 ****************************************************************************/

static FAR void *motion_thread(FAR void *arg)
{
  struct lsm6dsl_sensor_data_s raw;
  struct ai_watch_motion_snapshot_s *snap = &g_motion.snap;

  float grav_fx = 0.0f;         /* fast gravity LPF (tau ~0.4 s); the
                                 * pre-impact bearing is this vector
                                 * frozen at the impact sample */
  float grav_fy = 0.0f;
  float grav_fz = 1000.0f;      /* mg units: initial guess "face up" */
  float mag_mean = 1000.0f;     /* slow |a| mean */

  float dev_ring[DEV_RING_LEN];
  uint32_t dev_ring_ms[DEV_RING_LEN];
  unsigned dev_head = 0;

  uint32_t ff_seen_ms = 0;      /* last sample below the FF threshold;
                                 * 0 = none latched yet */

  bool step_armed = true;
  uint32_t last_step_ms = 0;

  float act_ring[ACT_RING_LEN];
  unsigned act_head = 0;
  unsigned act_count = 0;
  unsigned act_samples = 0;
  uint8_t act_candidate = AI_WATCH_MOTION_REST;
  unsigned act_cand_count = 0;

  bool soft_prev = false;
  uint32_t fall_window_end = 0;
  uint32_t cooldown_until = 0;
  float pre_ax = 0.0f;
  float pre_ay = 0.0f;
  float pre_az = 1000.0f;
  bool imp_ff = false;            /* free-fall preceded the impact */
  uint32_t imp_start_ms = 0;      /* impact time (observation budget) */
  uint32_t imp_last_move_ms = 0;  /* last sample dev > STILL_DEV */

  bool was_recording = false;
  uint32_t rec_t0 = 0;
  uint32_t rec_count = 0;

  int fd = -1;
  int ret;

  (void)arg;

  for (;;)
    {
      uint32_t now;
      float ax_g;
      float ay_g;
      float az_g;
      float mag;
      float dev;

      usleep(MOTION_PERIOD_MS * 1000);

      /* (Re-)open the sensor. The device is registered at boot; if it
       * ever disappears the retry keeps the rest of the watch alive.
       */

      if (fd < 0)
        {
          fd = open(LSM6DSL_DEV_PATH, O_RDONLY);
          if (fd < 0)
            {
              pthread_mutex_lock(&g_motion.lock);
              snap->sensor_ok = false;
              pthread_mutex_unlock(&g_motion.lock);
              usleep(500 * 1000);
              continue;
            }

          ret = ioctl(fd, SNIOC_START, 0);
          if (ret < 0)
            {
              close(fd);
              fd = -1;
              continue;
            }

          printf("MOTION: %s streaming (fixed 833 Hz ODR, poll "
                 "%d ms)\n", LSM6DSL_DEV_PATH, MOTION_PERIOD_MS);
        }

      ret = ioctl(fd, SNIOC_LSM6DSLSENSORREAD,
                  (unsigned long)(uintptr_t)&raw);
      now = motion_now_ms();
      if (ret < 0)
        {
          /* Bus error: drop the handle, reopen on a later pass */

          close(fd);
          fd = -1;
          pthread_mutex_lock(&g_motion.lock);
          snap->sensor_ok = false;
          pthread_mutex_unlock(&g_motion.lock);
          continue;
        }

      /* The driver converts to mg / mdps already */

      ax_g = (float)raw.x_data;
      ay_g = (float)raw.y_data;
      az_g = (float)raw.z_data;
      mag = sqrtf(ax_g * ax_g + ay_g * ay_g + az_g * az_g);
      dev = fabsf(mag - mag_mean);
      mag_mean += 0.01f * (mag - mag_mean);

      /* --- Gravity LPFs --- */

      grav_fx += 0.15f * (ax_g - grav_fx);
      grav_fy += 0.15f * (ay_g - grav_fy);
      grav_fz += 0.15f * (az_g - grav_fz);

      /* --- Recording capture --- */

      pthread_mutex_lock(&g_motion.lock);

      snap->last_mag_mg = (uint32_t)(mag + 0.5f);
      snap->sensor_ok = true;

      if (snap->recording)
        {
          struct motion_rec_sample_s s;

          if (!was_recording)
            {
              rec_t0 = now;
              rec_count = 0;
            }

          s.t_ms = now - rec_t0;
          s.x = raw.x_data;
          s.y = raw.y_data;
          s.z = raw.z_data;
          s.gx = raw.g_x_data;
          s.gy = raw.g_y_data;
          s.gz = raw.g_z_data;
          motion_rec_push(&s);

          rec_count++;
          snap->record_lines = rec_count;
        }
      was_recording = snap->recording;

      pthread_mutex_unlock(&g_motion.lock);

      /* --- Stillness deviation ring (last ~1 s) --- */

      dev_ring[dev_head] = dev;
      dev_ring_ms[dev_head] = now;
      dev_head = (dev_head + 1u) % DEV_RING_LEN;

      /* --- Free-fall latch: any single sample below the threshold
       * marks it; the 500 ms recency window in the impact rule keeps
       * stale latches irrelevant. At the ~15 Hz effective sample rate
       * a duration requirement would never latch on short falls. ---
       */

      if (mag < FALL_FREEFALL_MG)
        {
          ff_seen_ms = now;
        }

      /* --- Fall FSM + step / activity (steps and the activity window
       * only advance in NORMAL, so impact bounces cannot pollute them)
       * ---
       */

      pthread_mutex_lock(&g_motion.lock);

      switch (snap->fall_state)
        {
        case FALL_STATE_NORMAL:
          {
            bool impact;
            bool ff_recent;

            /* Free-fall counts only while it is fresh; ff_seen_ms == 0
             * means nothing latched since boot.
             */

            ff_recent = (ff_seen_ms != 0) &&
                        (now - ff_seen_ms <= FF_SETTLE_MS);

            impact = (mag > FALL_IMPACT_HARD_MG) ||
                     (soft_prev && mag > FALL_IMPACT_SOFT_MG) ||
                     (ff_recent && mag > FALL_IMPACT_SOFT_MG);
            soft_prev = (mag > FALL_IMPACT_SOFT_MG);

            if (impact && (int32_t)(now - cooldown_until) >= 0)
              {
                snap->fall_state = FALL_STATE_IMPACT;
                snap->fall_impact_mg = (int16_t)(mag + 0.5f);
                fall_window_end = now + FALL_STILL_WINDOW_MS;

                /* Freeze the current gravity direction as the
                 * pre-impact bearing: the fast LPF is converged after
                 * ~1 s, unlike the old slow filter that needed ~7 s. */

                pre_ax = grav_fx;
                pre_ay = grav_fy;
                pre_az = grav_fz;
                imp_ff = ff_recent;
                imp_start_ms = now;
                imp_last_move_ms = now;

                printf("MOTION: impact %d mg (ff=%s)\n",
                       (int)snap->fall_impact_mg,
                       ff_recent ? "yes" : "no");
              }

            /* Activity ring: always primed so classification resumes
             * immediately after any event ends; classification itself
             * only advances in NORMAL. */

            act_ring[act_head] = mag;
            act_head = (act_head + 1u) % ACT_RING_LEN;
            if (act_count < ACT_RING_LEN)
              {
                act_count++;
              }
            act_samples++;

            /* Steps */

            if (snap->fall_state == FALL_STATE_NORMAL)
              {
                if (step_armed)
                  {
                    if (mag > STEP_PEAK_MG &&
                        now - last_step_ms >= STEP_REFRACT_MS)
                      {
                        snap->steps_today++;
                        snap->distance_m =
                          (uint32_t)((uint64_t)snap->steps_today *
                                     STEP_STRIDE_MM / 1000u);
                        last_step_ms = now;
                        step_armed = false;
                      }
                  }
                else if (mag < STEP_VALLEY_MG)
                  {
                    step_armed = true;
                  }

                /* Activity classification */

                if (act_count == ACT_RING_LEN)
                  {
                    float sum = 0.0f;
                    float sq = 0.0f;
                    float std;
                    uint8_t cls;
                    unsigned i;

                    for (i = 0; i < ACT_RING_LEN; i++)
                      {
                        sum += act_ring[i];
                        sq += act_ring[i] * act_ring[i];
                      }

                    sum /= (float)ACT_RING_LEN;
                    sq = sq / (float)ACT_RING_LEN - sum * sum;
                    if (sq < 0.0f)
                      {
                        sq = 0.0f;
                      }

                    std = sqrtf(sq);
                    snap->activity_std_mg = (uint16_t)(std + 0.5f);

                    if (std < ACT_REST_STD_MG)
                      {
                        cls = AI_WATCH_MOTION_REST;
                      }
                    else if (std > ACT_RUN_STD_MG)
                      {
                        cls = AI_WATCH_MOTION_RUN;
                      }
                    else
                      {
                        cls = AI_WATCH_MOTION_WALK;
                      }

                    /* Debounce: publish after ~1 s of the same class */

                    if (cls == act_candidate)
                      {
                        act_cand_count++;
                      }
                    else
                      {
                        act_candidate = cls;
                        act_cand_count = 1;
                      }

                    if (act_cand_count >= ACT_DEBOUNCE_SAMPLES)
                      {
                        snap->activity = cls;
                        snap->activity_confident =
                          (act_samples > 100u) ? 1 : 0;
                      }
                  }
              }
          }
          break;

        case FALL_STATE_IMPACT:
          {
            if (dev > FALL_STILL_DEV_MG)
              {
                imp_last_move_ms = now;
              }

            if (mag > (float)snap->fall_impact_mg)
              {
                /* Keep the true peak for the report */

                snap->fall_impact_mg = (int16_t)(mag + 0.5f);
              }

            if (dev > FALL_JOLT_MG || mag > FALL_IMPACT_SOFT_MG)
              {
                /* Still bouncing: push the observation window out */

                fall_window_end = now + FALL_STILL_WINDOW_MS;
              }

            if ((int32_t)(now - fall_window_end) >= 0)
              {
                /* Was the last second still? */

                bool still = true;
                unsigned i;

                for (i = 0; i < DEV_RING_LEN; i++)
                  {
                    unsigned idx = (dev_head + DEV_RING_LEN - 1u - i) %
                                   DEV_RING_LEN;

                    if (now - dev_ring_ms[idx] > DEV_RING_MS)
                      {
                        break;
                      }

                    if (dev_ring[idx] > FALL_STILL_DEV_MG)
                      {
                        still = false;
                        break;
                      }
                  }

                if (!still)
                  {
                    printf("MOTION: impact followed by motion\n");
                    snap->fall_state = FALL_STATE_NORMAL;
                    cooldown_until = now + FALL_REJECT_COOLDOWN_MS;
                  }
                else
                  {
                    uint16_t ang = motion_angle_deg(pre_ax, pre_ay,
                                                    pre_az,
                                                    grav_fx, grav_fy,
                                                    grav_fz);

                    snap->fall_angle_deg =
                      (ang > 255) ? 255 : (uint8_t)ang;

                    if (ang >= FALL_ANGLE_MIN_DEG)
                      {
                        snap->fall_state = FALL_STATE_ALARM;
                        snap->fall_pending = 1;
                        snap->fall_deadline_ms =
                          now + AI_WATCH_FALL_COUNTDOWN_MS;
                        printf("MOTION: fall armed (ang=%u deg, "
                               "%d mg)\n", ang,
                               (int)snap->fall_impact_mg);
                      }
                    else if (imp_ff &&
                             now - imp_last_move_ms >=
                             FALL_LONG_STILL_MS)
                      {
                        /* Dropped flat without rotating: free-fall
                         * proves it was released, not placed, and the
                         * long stillness proves nobody picked it up.
                         */

                        snap->fall_state = FALL_STATE_ALARM;
                        snap->fall_pending = 1;
                        snap->fall_deadline_ms =
                          now + AI_WATCH_FALL_COUNTDOWN_MS;
                        printf("MOTION: fall armed (flat drop, still "
                               "%u ms, %d mg)\n",
                               (unsigned)(now - imp_last_move_ms),
                               (int)snap->fall_impact_mg);
                      }
                    else if (now - imp_start_ms < FALL_IMPACT_MAX_OBS_MS)
                      {
                        /* Still but neither path confirmed yet: keep
                         * watching in 1 s steps until the observation
                         * budget runs out.
                         */

                        fall_window_end = now + 1000;
                      }
                    else
                      {
                        printf("MOTION: impact but orientation kept "
                               "(%u deg)\n", ang);
                        snap->fall_state = FALL_STATE_NORMAL;
                        cooldown_until = now + FALL_REJECT_COOLDOWN_MS;
                      }
                  }
              }
          }
          break;

        case FALL_STATE_ALARM:
          if ((int32_t)(now - snap->fall_deadline_ms) >= 0)
            {
              uint8_t event = snap->fall_test ?
                              AI_WATCH_FALL_TEST :
                              AI_WATCH_FALL_CONFIRMED;

              snap->fall_pending = 0;
              snap->fall_state = FALL_STATE_NORMAL;
              cooldown_until = now + FALL_COOLDOWN_MS;
              motion_fall_push_locked(event,
                                      snap->fall_impact_mg,
                                      snap->fall_angle_deg);
              snap->fall_test = false;
            }
          break;
        }

      pthread_mutex_unlock(&g_motion.lock);
    }

  if (fd >= 0)
    {
      ioctl(fd, SNIOC_STOP, 0);
      close(fd);
    }

  return NULL;
}

/****************************************************************************
 * Private Functions - Main-loop drain / BLE
 ****************************************************************************/

static void motion_ble_maybe_send(void)
{
  enum ai_watch_ble_bsp_state_e st = ai_watch_ble_get_state();
  uint32_t now = motion_now_ms();
  uint32_t steps;
  uint8_t activity;
  uint8_t confident;

  if (st != AI_WATCH_BLE_BSP_CONNECTED)
    {
      g_ble_first_pass = true;
      return;
    }

  pthread_mutex_lock(&g_motion.lock);
  steps = g_motion.snap.steps_today;
  activity = g_motion.snap.activity;
  confident = g_motion.snap.activity_confident;
  pthread_mutex_unlock(&g_motion.lock);

  /* Steps: right after (re)connect, then on every change, at most
   * every 5 s. Posted to the TX worker - a failed attempt costs the
   * worker a backoff sleep, not the UI loop (the 4.8.0 first-pass
   * retry storm flooded the console until the CCCs were written).
   */

  if (g_ble_first_pass ||
      (steps != g_steps_sent && now - g_steps_sent_ms >= 5000))
    {
      if (ai_watch_ble_post(AI_WATCH_BLE_SENSOR_STEPS,
                            &steps, sizeof(uint32_t)) == 0)
        {
          g_steps_sent = steps;
          g_steps_sent_ms = now;
          g_ble_first_pass = false;
        }
    }

  /* Activity: on change only, at most every 5 s */

  if (confident && activity != g_activity_sent &&
      now - g_activity_sent_ms >= 5000)
    {
      if (ai_watch_ble_post(AI_WATCH_BLE_SENSOR_ACTIVITY,
                            &activity,
                            sizeof(uint8_t)) == 0)
        {
          g_activity_sent = activity;
          g_activity_sent_ms = now;
        }
    }
}

/* Main loop: print captured samples as CSV and forward a decimated
 * copy over BLE (sensor_type MOTION_DATA, 14-byte payload).
 */

static void motion_drain_recording(void)
{
  while (g_motion.rec_q_tail != g_motion.rec_q_head)
    {
      FAR struct motion_rec_sample_s *s =
        &g_motion.rec_q[g_motion.rec_q_tail];
      uint8_t payload[14];

      printf("REC,%u,%d,%d,%d,%d,%d,%d\n",
             (unsigned)s->t_ms, s->x, s->y, s->z, s->gx, s->gy, s->gz);

      g_rec_ble_seq++;
      if (g_rec_ble_seq % BLE_REC_DEC == 0)
        {
          memcpy(&payload[0], &s->t_ms, sizeof(uint32_t));
          memcpy(&payload[4], &s->x, 6 * sizeof(int16_t));
          ai_watch_ble_post(AI_WATCH_BLE_SENSOR_MOTION_DATA,
                            payload, sizeof(payload));
        }

      g_motion.rec_q_tail =
        (g_motion.rec_q_tail + 1u) % MOTION_REC_QLEN;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ai_watch_motion_init(void)
{
  pthread_attr_t attr;
  struct sched_param param;

  if (g_motion.thread_running)
    {
      return;
    }

  memset(&g_motion, 0, sizeof(g_motion));
  pthread_mutex_init(&g_motion.lock, NULL);
  g_motion.snap.fall_state = FALL_STATE_NORMAL;
  g_motion.snap.activity = AI_WATCH_MOTION_REST;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8192);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  param.sched_priority = 80;          /* below the LVGL loop (100) */
  pthread_attr_setschedparam(&attr, &param);

  if (pthread_create(&g_motion.thread, &attr, motion_thread, NULL) == 0)
    {
      g_motion.thread_running = true;
      printf("MOTION: service started\n");
    }
  else
    {
      printf("MOTION: ERROR thread create failed\n");
    }

  pthread_attr_destroy(&attr);
}

void ai_watch_motion_process(void)
{
  if (!g_motion.thread_running)
    {
      return;
    }

  motion_drain_recording();
  motion_ble_maybe_send();
}

void ai_watch_motion_get_snapshot(
    FAR struct ai_watch_motion_snapshot_s *out)
{
  pthread_mutex_lock(&g_motion.lock);
  *out = g_motion.snap;
  pthread_mutex_unlock(&g_motion.lock);
}

bool ai_watch_motion_record_start(void)
{
  bool ok = false;

  pthread_mutex_lock(&g_motion.lock);
  if (g_motion.snap.sensor_ok && !g_motion.snap.recording)
    {
      g_motion.snap.recording = true;
      g_motion.snap.record_lines = 0;
      ok = true;
    }
  pthread_mutex_unlock(&g_motion.lock);

  if (ok)
    {
      printf("MOTION: recording start\n");
    }

  return ok;
}

void ai_watch_motion_record_stop(void)
{
  pthread_mutex_lock(&g_motion.lock);
  if (g_motion.snap.recording)
    {
      g_motion.snap.recording = false;
      printf("MOTION: recording stop (%u lines)\n",
             (unsigned)g_motion.snap.record_lines);
    }
  pthread_mutex_unlock(&g_motion.lock);
}

bool ai_watch_motion_test_trigger(void)
{
  bool ok = false;

  pthread_mutex_lock(&g_motion.lock);
  if (g_motion.snap.fall_state == FALL_STATE_NORMAL &&
      !g_motion.snap.fall_pending)
    {
      g_motion.snap.fall_state = FALL_STATE_ALARM;
      g_motion.snap.fall_pending = 1;
      g_motion.snap.fall_deadline_ms =
        motion_now_ms() + AI_WATCH_FALL_COUNTDOWN_MS;
      g_motion.snap.fall_impact_mg = 3000;
      g_motion.snap.fall_angle_deg = 65;
      g_motion.snap.fall_test = true;
      ok = true;
    }
  pthread_mutex_unlock(&g_motion.lock);

  if (ok)
    {
      printf("MOTION: TEST fall countdown started\n");
    }

  return ok;
}

bool ai_watch_motion_fall_cancel(void)
{
  bool ok = false;
  int16_t impact = 0;
  uint8_t angle = 0;
  uint8_t event;

  pthread_mutex_lock(&g_motion.lock);
  if (g_motion.snap.fall_pending)
    {
      g_motion.snap.fall_pending = 0;
      g_motion.snap.fall_state = FALL_STATE_NORMAL;
      impact = g_motion.snap.fall_impact_mg;
      angle = g_motion.snap.fall_angle_deg;
      event = g_motion.snap.fall_test ?
              AI_WATCH_FALL_TEST : AI_WATCH_FALL_CANCELLED;
      g_motion.snap.fall_test = false;
      ok = true;
    }
  pthread_mutex_unlock(&g_motion.lock);

  if (ok)
    {
      printf("MOTION: fall alert cancelled\n");
      pthread_mutex_lock(&g_motion.lock);
      motion_fall_push_locked(event, impact, angle);
      pthread_mutex_unlock(&g_motion.lock);
    }

  return ok;
}

bool ai_watch_motion_poll_fall(FAR struct ai_watch_fall_report_s *out)
{
  bool have = false;

  if (g_motion.fall_q_tail != g_motion.fall_q_head)
    {
      struct ai_watch_fall_report_s rep;
      uint8_t payload[8];

      rep = g_motion.fall_q[g_motion.fall_q_tail];
      g_motion.fall_q_tail =
        (g_motion.fall_q_tail + 1u) % MOTION_FALL_QLEN;

      /* FALL_EVENT payload: event(1) + impact_mg(2, LE) + angle(1)
       * + reserved(4).
       */

      payload[0] = rep.event;
      memcpy(&payload[1], &rep.impact_mg, sizeof(uint16_t));
      payload[3] = rep.angle_deg;
      memset(&payload[4], 0, 4);

      ai_watch_ble_post(AI_WATCH_BLE_SENSOR_FALL_EVENT,
                        payload, sizeof(payload));

      if (out != NULL)
        {
          *out = rep;
        }

      have = true;
    }

  return have;
}
