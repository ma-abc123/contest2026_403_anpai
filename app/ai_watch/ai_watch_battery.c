/****************************************************************************
 * apps/ai_watch/ai_watch_battery.c
 *
 * Battery voltage and state of charge, read from the SoC's own GPADC.
 *
 * Hardware path (no extra parts, no kernel driver):
 *
 *   LiPo cell ──> AW32001 (charger) ──> 1:2 divider ──> GPADC channel 7
 *
 * The divider ratio, the channel number and the fact that the header's
 * ADC_CHAN_VBAT macro points at the wrong channel were all established by an
 * eight-channel scan across two boots (cell out / cell in); the evidence is
 * recorded next to ctx->channel in sf32lb_adc.c.
 *
 * Note that the cell really does power the board on its own: pulling the USB
 * cable while the watch is running leaves it running. It is only a cold start
 * on the cell alone that fails, which points at the power-on path rather than
 * at this measurement.
 *
 * This module only does open/ioctl/read on /dev/adc0, which keeps the kernel
 * image byte-identical and therefore cannot disturb the BLE path or the LCDC
 * timing.
 *
 * Two deliberate choices, both learned the hard way on this board:
 *   - One conversion blocks for ~256 ms (the driver averages 20 samples with a
 *     busy-wait between them), so sampling lives on its own low-priority
 *     worker instead of the LVGL thread. The worker touches only the ADC.
 *   - The battery is measured electrically, not through the charger. The
 *     AW32001 sits on PA10/PA11, but the board pinmux ends up assigning I2C2
 *     to PA40/PA39 for the IMU and the LTR-303, and an address scan of that
 *     bus finds 0x29, 0x30 and 0x6a but no 0x49. There is no charger to talk
 *     to on the bus the firmware actually owns.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>

#include <pthread.h>
#include <sched.h>

#include "ai_watch_battery.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Master switch for ADC access.
 *
 * 0 = touch neither /dev/adc0 nor the GPADC block. Used to isolate a BLE
 *     regression that appeared in the first battery build: HAL_ADC_Prepare()
 *     sets HPSYS_CFG.ANAU_CR.EN_VBAT_MON, and adc_read() then busy-waits
 *     ~200 ms per conversion (HAL_Delay_us is a spin loop, not sleep).
 * 1 = normal sampling.
 */

#define BATTERY_SAMPLING_ENABLE  1

#define BATTERY_ADC_DEV       "/dev/adc0"


/* Sampling cadence. Deliberately slow: one conversion busy-waits ~200 ms
 * inside HAL_Delay_us, so 60 s keeps the duty cycle at ~0.3%. A watch cell
 * moves by a few mV per minute anyway.
 */

#define BATTERY_PERIOD_MS     60000

/* Worker priority. NuttX counts up: a larger number runs first.
 *
 * This used to ask for 110 while the comment claimed it sat "below motion (80)
 * and light (90)". 110 is above all of them -- above the LVGL loop (100), the
 * light worker (90) and motion (80). A conversion busy-waits for ~256 ms
 * inside HAL_Delay_us(), which is a spin loop rather than a sleep, so at 110
 * the worker could hold the CPU for a quarter of a second with every other
 * thread queued behind it. The first extra sample lands 60 s after boot,
 * right about when a phone has finished connecting, and an HCI command or a
 * connection event that misses its window by that much is a dropped link.
 *
 * 50 is deliberately below motion (80). Nothing the battery module produces
 * is worth delaying a sensor sample, and a late reading costs nothing: the
 * cell moves by a few mV per minute.
 */

#define BATTERY_PRIORITY     50

/* Plausible window for the divider output, in the driver's millivolt scale.
 *
 * The cell can only sit between the protection cutoff (~2.5 V) and charge
 * termination (~4.35 V), so the pin sees roughly 1250..2175 mV. Anything
 * outside that is a failed conversion, not a measurement -- in particular
 * sf32lb_adc.c returns 0 from adc_read() when HAL_ADC_PollForConversion()
 * times out, and 0 mV at the pin would otherwise be averaged in as a flat
 * cell and pin the gauge at 0%.
 */

#define BATTERY_PIN_MIN_MV   1000
#define BATTERY_PIN_MAX_MV   2300

/* Moving average depth. The panel, BLE and the motion worker all draw current
 * in bursts, so a single sample can sag by 100 mV or more.
 */

#define BATTERY_FILTER_N      8

/* Divider between the cell and the GPADC input, times 100.
 *
 * The board puts the cell behind a 1:2 divider, so the pin never sees more
 * than ~2.1 V and stays clear of the ~3.5 V full scale the driver's own
 * calibration implies. With a cell fitted, channel 7 read 2044 mV at the pin
 * (2736 raw); doubling that gives 4088 mV. It had read 2018 mV five minutes
 * earlier, and the slow climb is what a cell taking charge looks like.
 *
 * Two supporting observations, both from the same board:
 *   - with the cell removed the pin floats up to 2233 mV (x2 = 4466 mV,
 *     i.e. the charger's BAT pin with no load on it), which is exactly the
 *     direction a divider sitting on that node should move;
 *   - the numbers did not change at all on channel 5, which the header calls
 *     "internal VBAT" -- that is why this module does not use it.
 *
 * Re-check against a multimeter before trusting the percentage; adjust this
 * constant only if a measurement disagrees.
 */

#define BATTERY_DIVIDER_X100  200

/* Resting-voltage table. A single LiPo cell is ~4.20 V full and ~3.30 V
 * empty, but the curve between them is far from linear: it is flat through
 * the middle third, which is exactly where a naive linear map gets it wrong.
 */

struct battery_point_s
{
  int mv;
  int percent;
};

static const struct battery_point_s g_battery_curve[] =
{
  { 4200, 100 },
  { 4130,  95 },
  { 4060,  88 },
  { 3990,  80 },
  { 3930,  72 },
  { 3870,  64 },
  { 3820,  56 },
  { 3780,  48 },
  { 3750,  40 },
  { 3720,  32 },
  { 3690,  24 },
  { 3650,  17 },
  { 3600,  10 },
  { 3500,   5 },
  { 3300,   0 },
};

#define BATTERY_CURVE_LEN \
  (sizeof(g_battery_curve) / sizeof(g_battery_curve[0]))

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int      g_fd = -1;
static int      g_raw_mv = -1;
static int      g_vbat_mv = -1;
static int      g_percent = -1;
static bool     g_available;

static int      g_hist[BATTERY_FILTER_N];
static int      g_hist_len;
static int      g_hist_pos;

static pthread_t g_worker;
static bool      g_worker_started;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int battery_percent_from_mv(int mv)
{
  int i;

  if (mv >= g_battery_curve[0].mv)
    {
      return 100;
    }

  if (mv <= g_battery_curve[BATTERY_CURVE_LEN - 1].mv)
    {
      return 0;
    }

  for (i = 1; i < (int)BATTERY_CURVE_LEN; i++)
    {
      if (mv >= g_battery_curve[i].mv)
        {
          const struct battery_point_s *hi = &g_battery_curve[i - 1];
          const struct battery_point_s *lo = &g_battery_curve[i];
          int span = hi->mv - lo->mv;
          int drop = hi->percent - lo->percent;

          if (span <= 0)
            {
              return lo->percent;
            }

          return lo->percent + (mv - lo->mv) * drop / span;
        }
    }

  return 0;
}

static int battery_hist_average(void)
{
  long sum = 0;
  int i;

  if (g_hist_len == 0)
    {
      return -1;
    }

  for (i = 0; i < g_hist_len; i++)
    {
      sum += g_hist[i];
    }

  return (int)(sum / g_hist_len);
}

static void battery_hist_push(int mv)
{
  g_hist[g_hist_pos] = mv;
  g_hist_pos = (g_hist_pos + 1) % BATTERY_FILTER_N;

  if (g_hist_len < BATTERY_FILTER_N)
    {
      g_hist_len++;
    }
}

/****************************************************************************
 * Name: battery_read_raw
 *
 * Description:
 *   One blocking conversion through the generic ADC character driver. Returns
 *   the value in the driver's own millivolt scale, before the divider is
 *   applied.
 *
 ****************************************************************************/

static int battery_read_raw(int *out_mv)
{
  struct adc_msg_s sample;
  ssize_t nread;
  int ret;

  if (g_fd < 0)
    {
      g_fd = open(BATTERY_ADC_DEV, O_RDONLY);
      if (g_fd < 0)
        {
          return -errno;
        }
    }

  ret = ioctl(g_fd, ANIOC_TRIGGER, 0);
  if (ret < 0)
    {
      ret = -errno;
      goto errout;
    }

  memset(&sample, 0, sizeof(sample));
  nread = read(g_fd, &sample, sizeof(sample));
  if (nread != (ssize_t)sizeof(sample))
    {
      ret = nread < 0 ? -errno : -EIO;
      goto errout;
    }

  /* Drop implausible conversions instead of averaging them in. The handle
   * stays open here on purpose: the device is fine, the sample is not, and
   * reopening costs another ADC setup for nothing.
   */

  if (sample.am_data < BATTERY_PIN_MIN_MV ||
      sample.am_data > BATTERY_PIN_MAX_MV)
    {
      return -ERANGE;
    }

  *out_mv = (int)sample.am_data;
  return OK;

errout:
  /* Drop the handle so the next call reopens it; the ADC device itself is
   * registered once at boot and is never torn down.
   */

  close(g_fd);
  g_fd = -1;
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: battery_worker
 *
 * Description:
 *   Sampling lives on its own thread because one conversion blocks for about
 *   256 ms: sf32lb_adc.c averages ADC_AVERAGE_COUNT (20) samples with a fixed
 *   HAL_Delay_us(10 * 1000) between them. Doing that on the LVGL thread would
 *   freeze the UI for a quarter second every period.
 *
 *   The spawn priority is BATTERY_PRIORITY, which has to sit genuinely below
 *   motion (80) and light (90) -- see the comment on that macro. This thread
 *   touches only the ADC, so it cannot delay the sensor bus, but the spin
 *   inside HAL_Delay_us() means it can delay anything below it if it is
 *   allowed to run high.
 *
 ****************************************************************************/

static void *battery_worker(void *arg)
{
  (void)arg;

  for (;;)
    {
      int raw;
      int ret;

      ret = battery_read_raw(&raw);
      if (ret >= 0)
        {
          g_raw_mv = raw;
          g_vbat_mv = raw * BATTERY_DIVIDER_X100 / 100;
          battery_hist_push(g_vbat_mv);
          g_percent = battery_percent_from_mv(battery_hist_average());
          g_available = true;

          /* Print every sample. At 60 s intervals this is quiet, and it is
           * what makes a battery plug/unplug visible within a minute.
           */

          printf("BAT: raw=%d mV vbat=%d mV soc=%d%% (n=%d)\n",
                 g_raw_mv, g_vbat_mv, g_percent, g_hist_len);
        }
      else if (ret == -ERANGE)
        {
          /* Logged rather than swallowed: a stream of these means the ADC is
           * timing out, and the gauge would otherwise just look stuck.
           */

          printf("BAT: implausible sample dropped\n");
        }

      usleep(BATTERY_PERIOD_MS * 1000);
    }

  return NULL;
}

void ai_watch_battery_init(void)
{
#if !BATTERY_SAMPLING_ENABLE
  /* Isolation build: report nothing, touch nothing. The Home gauge then
   * shows "--%" because ai_watch_battery_percent() returns -1.
   */

  printf("BAT: sampling disabled (BLE isolation build)\n");
  return;
#else
  pthread_attr_t attr;
  struct sched_param param;

  if (g_worker_started)
    {
      return;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  param.sched_priority = BATTERY_PRIORITY;
  pthread_attr_setschedparam(&attr, &param);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  if (pthread_create(&g_worker, &attr, battery_worker, NULL) == 0)
    {
      g_worker_started = true;
      printf("BAT: ADC worker started (priority=%d, period=%u ms)\n",
             BATTERY_PRIORITY, BATTERY_PERIOD_MS);
    }
  else
    {
      printf("BAT: worker create failed\n");
    }

  pthread_attr_destroy(&attr);
#endif /* BATTERY_SAMPLING_ENABLE */
}

void ai_watch_battery_poll(void)
{
  /* Nothing to do: sampling runs on the worker. The hook is kept so the main
   * loop does not have to know that.
   */
}

int ai_watch_battery_mv(void)
{
  return g_available ? g_vbat_mv : -1;
}

int ai_watch_battery_percent(void)
{
  return g_available ? g_percent : -1;
}

bool ai_watch_battery_available(void)
{
  return g_available;
}
