/****************************************************************************
 * apps/examples/ai_watch/ai_watch_skill.c
 *
 * Device-side Skill layer. See ai_watch_skill.h for the model.
 *
 * Threading: one worker at AI_WATCH_SKILL_TICK_MS. It only reads the
 * motion snapshot (mutex protected inside the motion service) and pushes
 * events into a small ring the main loop drains. No LVGL call and no BLE
 * call happens on this thread, which keeps it clear of both the display
 * and the radio path -- the two things that turned out to be fragile in
 * earlier rounds.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ai_watch_motion.h"
#include "ai_watch_skill.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* How often the evaluator wakes up. Cheap: it reads a snapshot. */

#define AI_WATCH_SKILL_TICK_MS      1000

/* Continuous sitting before a reminder. The counter is reset by any
 * sample classified as walking or running, so standing still for a moment
 * does not restart it -- only real movement does.
 */

#define SIT_LIMIT_MS                (50 * 60 * 1000)
#define SIT_REPEAT_MS               (30 * 60 * 1000)

/* Daily brief slot, local hours. Fires on the first tick at or after it
 * while the watch is awake; a brief missed while powered off is not
 * replayed, which keeps the negative screen honest.
 */

#define DAILY_BRIEF_HOUR            21

/* Demo timings. Short enough to show on stage, long enough to see. */

#define DEMO_SIT_LIMIT_MS           (20 * 1000)
#define DEMO_SIT_REPEAT_MS          (20 * 1000)
#define DEMO_GOAL_STEPS             20

#define DEFAULT_GOAL_STEPS          6000

#define SKILL_QUEUE_LEN             8

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ai_watch_skill_state_s
{
  pthread_t      thread;
  bool           started;
  bool           demo;
  uint32_t       goal_steps;

  /* Sitting tracker */

  uint32_t       sit_ms;
  uint32_t       sit_repeat_ms;

  /* Goal latch: fires once per day unless re-armed */

  bool           goal_fired;
  bool           need_reset;

  /* Daily brief: last local day-of-year that produced a brief, -1 = none */

  int            brief_day;

  /* Event queue, single producer (worker) / single consumer (main) */

  pthread_mutex_t lock;
  FAR struct ai_watch_skill_event_s queue[SKILL_QUEUE_LEN];
  uint8_t        head;
  uint8_t        tail;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct ai_watch_skill_state_s g_skill =
{
  .brief_day = -1,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void skill_push(FAR const struct ai_watch_skill_event_s *ev)
{
  uint8_t next;

  pthread_mutex_lock(&g_skill.lock);

  next = (g_skill.tail + 1) % SKILL_QUEUE_LEN;
  if (next != g_skill.head)
    {
      g_skill.queue[g_skill.tail] = *ev;
      g_skill.tail = next;
    }

  /* On overflow the newest event is dropped rather than overwriting a
   * queued one: a stale "you have been sitting" is worse than a missing
   * one, and the repeat timer will produce another soon.
   */

  pthread_mutex_unlock(&g_skill.lock);
}

static void skill_local_day(FAR struct tm *out)
{
  time_t now = time(NULL);

  localtime_r(&now, out);
}

static uint16_t skill_day_key(FAR const struct tm *tm)
{
  return (uint16_t)(tm->tm_yday + 1);
}

static FAR void *skill_thread(FAR void *arg)
{
  FAR struct ai_watch_skill_state_s *st = (FAR struct ai_watch_skill_state_s *)arg;
  int last_brief_day = -1;

  for (;;)
    {
      struct ai_watch_motion_snapshot_s snap;
      struct ai_watch_skill_event_s ev;
      struct tm tm;
      uint32_t sit_limit;
      uint32_t sit_repeat;

      usleep(AI_WATCH_SKILL_TICK_MS * 1000);

      memset(&snap, 0, sizeof(snap));
      ai_watch_motion_get_snapshot(&snap);

      sit_limit  = st->demo ? DEMO_SIT_LIMIT_MS  : SIT_LIMIT_MS;
      sit_repeat = st->demo ? DEMO_SIT_REPEAT_MS : SIT_REPEAT_MS;

      /* Sitting: accumulate unless the classifier saw movement. */

      if (snap.sensor_ok && snap.activity_confident &&
          (snap.activity == AI_WATCH_MOTION_WALK ||
           snap.activity == AI_WATCH_MOTION_RUN))
        {
          if (st->sit_ms >= sit_limit)
            {
              /* Just stood up after a long sit: no reminder needed. */
            }

          st->sit_ms = 0;
          st->sit_repeat_ms = 0;
        }
      else
        {
          st->sit_ms += AI_WATCH_SKILL_TICK_MS;

          if (st->sit_ms >= sit_limit &&
              st->sit_repeat_ms >= sit_repeat)
            {
              memset(&ev, 0, sizeof(ev));
              ev.type   = AI_WATCH_SKILL_EVENT_SIT;
              ev.demo   = st->demo;
              ev.steps  = snap.steps_today;
              ev.sit_ms = st->sit_ms;
              skill_push(&ev);

              st->sit_repeat_ms = 0;
            }

          if (st->sit_repeat_ms < sit_repeat)
            {
              st->sit_repeat_ms += AI_WATCH_SKILL_TICK_MS;
            }
        }

      /* Step goal: edge triggered, re-armed at the day rollover. */

      if (st->need_reset)
        {
          st->need_reset = false;
          st->goal_fired = false;
        }

      if (!st->goal_fired)
        {
          uint32_t goal = st->goal_steps;

          if (st->demo && goal > DEMO_GOAL_STEPS)
            {
              goal = DEMO_GOAL_STEPS;
            }

          if (goal > 0 && snap.steps_today >= goal)
            {
              memset(&ev, 0, sizeof(ev));
              ev.type     = AI_WATCH_SKILL_EVENT_GOAL;
              ev.demo     = st->demo;
              ev.steps    = snap.steps_today;
              ev.goal_pct = 100;
              skill_push(&ev);

              st->goal_fired = true;
            }
        }

      /* Daily brief: first tick at or after the slot, once per local day.
       * Suppressed when the wall clock is not yet set (1970), otherwise a
       * fresh boot with no phone would fire immediately.
       */

      skill_local_day(&tm);

      if (tm.tm_year + 1900 >= 2024 && tm.tm_hour >= DAILY_BRIEF_HOUR)
        {
          int day = skill_day_key(&tm);

          if (day != st->brief_day && day != last_brief_day)
            {
              memset(&ev, 0, sizeof(ev));
              ev.type  = AI_WATCH_SKILL_EVENT_BRIEF;
              ev.demo  = st->demo;
              ev.steps = snap.steps_today;
              skill_push(&ev);

              st->brief_day = day;
              last_brief_day = day;
            }
        }
    }

  return (FAR void *)0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ai_watch_skill_init(uint32_t goal_steps)
{
  if (g_skill.started)
    {
      return 0;
    }

  g_skill.goal_steps = goal_steps > 0 ? goal_steps : DEFAULT_GOAL_STEPS;

  pthread_mutex_init(&g_skill.lock, NULL);

  if (pthread_create(&g_skill.thread, NULL, skill_thread, &g_skill) != 0)
    {
      pthread_mutex_destroy(&g_skill.lock);
      return -EAGAIN;
    }

  g_skill.started = true;
  printf("SKILL: %d device skills armed (goal=%u steps)\n",
         3, (unsigned)g_skill.goal_steps);
  return 0;
}

bool ai_watch_skill_poll(FAR struct ai_watch_skill_event_s *out)
{
  bool got = false;

  pthread_mutex_lock(&g_skill.lock);

  if (g_skill.head != g_skill.tail)
    {
      *out = g_skill.queue[g_skill.head];
      g_skill.head = (g_skill.head + 1) % SKILL_QUEUE_LEN;
      got = true;
    }

  pthread_mutex_unlock(&g_skill.lock);

  return got;
}

void ai_watch_skill_set_demo(bool on)
{
  pthread_mutex_lock(&g_skill.lock);
  g_skill.demo = on;

  /* Restart the sitting window so demo mode shows its effect immediately
   * instead of waiting out a real 50 minute sit.
   */

  g_skill.sit_ms = 0;
  g_skill.sit_repeat_ms = 0;
  g_skill.goal_fired = false;
  pthread_mutex_unlock(&g_skill.lock);

  printf("SKILL: demo mode %s\n", on ? "ON" : "off");
}

bool ai_watch_skill_demo_active(void)
{
  return g_skill.demo;
}

void ai_watch_skill_set_goal(uint32_t goal_steps)
{
  g_skill.goal_steps = goal_steps > 0 ? goal_steps : DEFAULT_GOAL_STEPS;
  g_skill.goal_fired = false;
}

void ai_watch_skill_reset_goal_latch(void)
{
  g_skill.need_reset = true;
}
