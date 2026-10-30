/****************************************************************************
 * apps/examples/ai_watch/ai_watch_skill.h
 *
 * Device-side Skill layer: declarative trigger -> action rules that run
 * on the watch itself, without a phone in the loop.
 *
 * The phone LLM is untouched by this layer. Its only role here is the
 * optional text generation for the daily brief: when a skill fires it
 * posts a structured event upward, and the phone may answer with a
 * DAILY_BRIEF command carrying one sentence of commentary. If no phone
 * is connected, the skill still runs and the watch shows local data.
 *
 * Three reference skills ship with the firmware:
 *   goal-cheer    - steps reach the daily goal            (event driven)
 *   sit-reminder  - no walking for SIT_LIMIT_MS           (event driven)
 *   daily-brief   - once per day at DAILY_BRIEF_HOUR       (time driven)
 *
 * Demo mode (`skill demo on`) shortens every threshold so a stage demo
 * does not have to wait an hour. It is off by default and never
 * persisted, so a watch that is merely rebooted stays in real timings.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_AI_WATCH_AI_WATCH_SKILL_H
#define __APPS_EXAMPLES_AI_WATCH_AI_WATCH_SKILL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A skill event surfaced to the main loop. The worker never touches LVGL;
 * the loop drains these through ai_watch_skill_poll() and renders them.
 */

#define AI_WATCH_SKILL_EVENT_SIT        1  /* been sitting too long */
#define AI_WATCH_SKILL_EVENT_GOAL       2  /* daily step goal reached */
#define AI_WATCH_SKILL_EVENT_BRIEF      3  /* daily brief slot is due */

/* Comment attached to an event produced with demo timings. The UI uses it
 * to label the banner so a demo never looks like a real reminder.
 */

#define AI_WATCH_SKILL_DEMO_NOTE        "demo"

struct ai_watch_skill_event_s
{
  uint8_t type;                 /* AI_WATCH_SKILL_EVENT_* */
  bool demo;                    /* produced under demo timings */
  uint32_t steps;               /* steps when the event fired */
  uint32_t sit_ms;              /* ms of continuous sitting */
  uint8_t goal_pct;             /* steps / goal, clamped to 255 */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: ai_watch_skill_init
 *
 * Description:
 *   Build the skill table and start the evaluator thread. Idempotent.
 *
 * Input Parameters:
 *   goal_steps - daily step goal, or 0 for the built-in default.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

int ai_watch_skill_init(uint32_t goal_steps);

/****************************************************************************
 * Name: ai_watch_skill_poll
 *
 * Description:
 *   Pop one pending skill event. Called from the main loop only.
 *
 * Returned Value:
 *   true when an event was returned; false when the queue is empty.
 *
 ****************************************************************************/

bool ai_watch_skill_poll(FAR struct ai_watch_skill_event_s *out);

/****************************************************************************
 * Name: ai_watch_skill_set_demo / _demo_active
 *
 * Description:
 *   Switch to compressed thresholds for a live demo, or back.
 *
 ****************************************************************************/

void ai_watch_skill_set_demo(bool on);
bool ai_watch_skill_demo_active(void);

/****************************************************************************
 * Name: ai_watch_skill_set_goal
 *
 * Description:
 *   Change the daily step goal at runtime (settings page).
 *
 ****************************************************************************/

void ai_watch_skill_set_goal(uint32_t goal_steps);

/****************************************************************************
 * Name: ai_watch_skill_reset_goal_latch
 *
 * Description:
 *   Re-arm the goal-reached trigger, called at the local midnight rollover.
 *
 ****************************************************************************/

void ai_watch_skill_reset_goal_latch(void);

#endif /* __APPS_EXAMPLES_AI_WATCH_AI_WATCH_SKILL_H */
