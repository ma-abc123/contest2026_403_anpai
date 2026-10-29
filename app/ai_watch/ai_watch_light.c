/****************************************************************************
 * apps/ai_watch/ai_watch_light.c
 *
 * Ambient light + brightness.
 *
 * The LTR-303ALS-01 sits on I2C1 at 0x29 and shares the sensor rail with the
 * IMU (PA30). There is no in-tree driver that fits it (ltr308_uorb.c talks to
 * a different register map), so this module drives it from user space through
 * the generic I2C character driver. That keeps the kernel untouched: nothing
 * here can move a section, change a heap boundary or disturb the BLE path.
 *
 * Brightness goes out through LCDDEVIO_SETPOWER on /dev/lcd0, which reaches
 * sf32lb_lcd_setpower() and then the CO5300's own brightness register.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/lcd/lcd_dev.h>

#include "ai_watch_light.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LIGHT_I2C_DEV       "/dev/i2c1"
#define LIGHT_LCD_DEV       "/dev/lcd0"
#define LIGHT_I2C_FREQ      400000

#define LTR303_ADDR         0x29

/* Register map (LTR-303ALS-01 datasheet DS86-2013-0004). Confirmed against a
 * real 0x80..0x8F dump on this board, which matched the factory defaults
 * byte for byte: CONTR 0x00, MEAS_RATE 0x03, PART_ID 0xA0, MANUFAC_ID 0x05,
 * INTERRUPT 0x08. Note MEAS_RATE lives at 0x85, not 0x81, and there is a
 * hole at 0x81..0x84.
 */

#define LTR303_ALS_CONTR    0x80
#define LTR303_MEAS_RATE    0x85
#define LTR303_PART_ID      0x86
#define LTR303_MANU_ID      0x87
#define LTR303_DATA_CH1_0   0x88   /* CH1_0, CH1_1, CH0_0, CH0_1 */
#define LTR303_DATA_CH0_0   0x8A

#define LTR303_PART_ID_VAL  0xA0
#define LTR303_MANU_ID_VAL  0x05

/* ALS_CONTR: gain 1x (bits 2:1 = 00) | active mode (bit 0 = 1).
 * MEAS_RATE: integration 100 ms (bits 5:3 = 000) | repeat 500 ms (bits 2:0 = 011).
 */

#define LTR303_CONTR_ACTIVE 0x01
#define LTR303_MEAS_500MS   0x03

#define LIGHT_PERIOD_MS     1000
#define LIGHT_STACKSIZE     3072
#define LIGHT_PRIORITY      90            /* below UI (100), above motion (80) */

/* Manual range, in percent of panel maximum. The settings bar exposes the
 * full 1..100 span; light_apply() clamps whatever it is handed.
 */

#define LIGHT_LEVEL_MAX     100

/* Auto-brightness bands: [lux above] -> percent. */

static const struct
{
  uint32_t lux;
  uint8_t  percent;
} g_light_bands[] =
{
  {    0, 20 },   /* dark room */
  {    5, 35 },
  {   50, 55 },
  {  200, 75 },
  {  800, 100 },  /* daylight */
};

#define LIGHT_NBANDS (sizeof(g_light_bands) / sizeof(g_light_bands[0]))

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_i2c_fd = -1;
static int g_lcd_fd = -1;
static bool g_present;
static bool g_auto = true;
static int g_level = LIGHT_LEVEL_MAX;

/* Level decided by the worker but not yet written to the panel. The write
 * itself must happen on the thread that owns the display: the LCD driver
 * (sf32lb_lcd.c / co5300.c) has no lock at all, so an ioctl from the worker
 * can reconfigure the LCDC in the middle of a frame flush.
 */

static int g_pending_level = -1;
static uint32_t g_lux;
static pthread_t g_thread;
static bool g_thread_running;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: light_read_regs
 *
 * Description:
 *   Two-phase register read: address phase, then a data phase with the
 *   sensor's own pointer auto-incrementing. Deliberately not using the
 *   I2C_M_NOSTOP/I2C_M_NOSTART single-transfer form: the two-phase pattern
 *   is what the in-tree IMU driver has always exercised on this SoC.
 *
 ****************************************************************************/

static int light_read_regs(uint8_t reg, FAR uint8_t *buf, size_t len)
{
  struct i2c_msg_s msg;
  struct i2c_transfer_s xfer;
  int ret;

  if (g_i2c_fd < 0)
    {
      return -ENODEV;
    }

  msg.addr      = LTR303_ADDR;
  msg.frequency = LIGHT_I2C_FREQ;
  msg.flags     = 0;
  msg.buffer    = &reg;
  msg.length    = 1;

  xfer.msgv = &msg;
  xfer.msgc = 1;

  ret = ioctl(g_i2c_fd, I2CIOC_TRANSFER, (unsigned long)(uintptr_t)&xfer);
  if (ret < 0)
    {
      return ret;
    }

  msg.flags  = I2C_M_READ;
  msg.buffer = buf;
  msg.length = len;

  ret = ioctl(g_i2c_fd, I2CIOC_TRANSFER, (unsigned long)(uintptr_t)&xfer);
  if (ret < 0)
    {
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: light_write_reg
 ****************************************************************************/

static int light_write_reg(uint8_t reg, uint8_t value)
{
  struct i2c_msg_s msg;
  struct i2c_transfer_s xfer;
  uint8_t buf[2];
  int ret;

  if (g_i2c_fd < 0)
    {
      return -ENODEV;
    }

  buf[0] = reg;
  buf[1] = value;

  msg.addr      = LTR303_ADDR;
  msg.frequency = LIGHT_I2C_FREQ;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;

  xfer.msgv = &msg;
  xfer.msgc = 1;

  ret = ioctl(g_i2c_fd, I2CIOC_TRANSFER, (unsigned long)(uintptr_t)&xfer);
  return ret < 0 ? ret : OK;
}

/****************************************************************************
 * Name: light_apply
 *
 * Description:
 *   Push a percentage to the panel. Also used at boot to replace the
 *   panel's power-on default.
 *
 ****************************************************************************/

static void light_apply(int percent)
{
  if (percent < 1)
    {
      percent = 1;
    }

  if (percent > 100)
    {
      percent = 100;
    }

  if (g_lcd_fd >= 0)
    {
      ioctl(g_lcd_fd, LCDDEVIO_SETPOWER, (unsigned long)percent);
    }

  g_level = percent;
}

/****************************************************************************
 * Name: light_level_for_lux
 ****************************************************************************/

static int light_level_for_lux(uint32_t lux)
{
  unsigned i;
  int percent = g_light_bands[0].percent;

  for (i = 0; i < LIGHT_NBANDS; i++)
    {
      if (lux >= g_light_bands[i].lux)
        {
          percent = g_light_bands[i].percent;
        }
    }

  return percent;
}

/****************************************************************************
 * Name: light_measure
 *
 * Description:
 *   One ALS conversion. Returns the computed lux, or a negative errno.
 *
 *   CH0 counts visible+IR, CH1 counts IR only. The ratio picks the
 *   compensation curve, which is the standard LTR-303 formula for gain 1x
 *   and 100 ms integration.
 *
 ****************************************************************************/

static int light_measure(FAR uint32_t *lux)
{
  uint8_t raw[4];
  uint16_t ch1;
  uint16_t ch0;
  float ratio;
  float value;
  int ret;

  ret = light_read_regs(LTR303_DATA_CH1_0, raw, sizeof(raw));
  if (ret < 0)
    {
      return ret;
    }

  ch1 = (uint16_t)((raw[1] << 8) | raw[0]);
  ch0 = (uint16_t)((raw[3] << 8) | raw[2]);

  if (ch0 == 0 && ch1 == 0)
    {
      *lux = 0;
      return OK;
    }

  ratio = (float)ch1 / (float)(ch0 + ch1);

  if (ratio < 0.45f)
    {
      value = 1.7743f * ch0 + 1.1059f * ch1;
    }
  else if (ratio < 0.64f)
    {
      value = 4.2785f * ch0 - 1.9548f * ch1;
    }
  else if (ratio < 0.85f)
    {
      value = 0.5926f * ch0 + 0.1185f * ch1;
    }
  else
    {
      value = 0.0f;
    }

  *lux = (uint32_t)(value + 0.5f);
  return OK;
}

/****************************************************************************
 * Name: light_thread
 ****************************************************************************/

static FAR void *light_thread(FAR void *arg)
{
  int candidate = -1;
  unsigned stable = 0;

  (void)arg;

  for (;;)
    {
      uint32_t lux;
      int level;

      usleep(LIGHT_PERIOD_MS * 1000);

      if (!g_present)
        {
          continue;
        }

      if (light_measure(&lux) < 0)
        {
          continue;
        }

      g_lux = lux;

      if (!g_auto)
        {
          continue;
        }

      level = light_level_for_lux(lux);

      /* Require two consecutive samples in the same band so a hand passing
       * over the sensor does not make the panel flash.
       */

      if (level == candidate)
        {
          stable++;
        }
      else
        {
          candidate = level;
          stable = 1;
        }

      if (stable >= 2 && level != g_level && level != g_pending_level)
        {
          g_pending_level = level;
        }
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ai_watch_light_init(void)
{
  uint8_t dump[16];
  uint8_t part;
  uint8_t manu;
  int ret;

  g_i2c_fd = open(LIGHT_I2C_DEV, O_RDWR | O_CLOEXEC);
  if (g_i2c_fd < 0)
    {
      printf("LIGHT: %s open failed: %d\n", LIGHT_I2C_DEV, errno);
      return -errno;
    }

  g_lcd_fd = open(LIGHT_LCD_DEV, O_RDWR | O_CLOEXEC);
  if (g_lcd_fd < 0)
    {
      printf("LIGHT: %s open failed: %d\n", LIGHT_LCD_DEV, errno);
    }

  /* Dump the identification page first: this is the gate for the whole
   * feature. If the IDs do not match, nothing else is touched.
   */

  ret = light_read_regs(LTR303_ALS_CONTR, dump, sizeof(dump));
  if (ret < 0)
    {
      printf("LIGHT: no answer at 0x%02x (%d)\n", LTR303_ADDR, ret);
      return ret;
    }

  printf("LIGHT: 0x80..0x8F =");
  for (int i = 0; i < (int)sizeof(dump); i++)
    {
      printf(" %02x", dump[i]);
    }

  printf("\n");

  part = dump[LTR303_PART_ID - LTR303_ALS_CONTR];
  manu = dump[LTR303_MANU_ID - LTR303_ALS_CONTR];

  if (part != LTR303_PART_ID_VAL || manu != LTR303_MANU_ID_VAL)
    {
      printf("LIGHT: unexpected IDs part=0x%02x manu=0x%02x, disabled\n",
             part, manu);
      return -ENODEV;
    }

  printf("LIGHT: LTR-303ALS-01 detected (part=0x%02x manu=0x%02x)\n",
         part, manu);

  g_present = true;

  /* Enable the ALS: active mode, gain 1x, 100 ms integration, 500 ms repeat */

  ret = light_write_reg(LTR303_MEAS_RATE, LTR303_MEAS_500MS);
  if (ret >= 0)
    {
      ret = light_write_reg(LTR303_ALS_CONTR, LTR303_CONTR_ACTIVE);
    }

  if (ret < 0)
    {
      printf("LIGHT: enable failed: %d\n", ret);
      g_present = false;
      return ret;
    }

  /* The sampling thread is deliberately NOT created here any more; see
   * ai_watch_light_start() below. The thread waits one full sample period
   * before reading, which gives the sensor's first conversion time to settle
   * without blocking the caller.
   */

  /* Boot default: auto brightness on, with a sane level until the first
   * conversion lands.
   */

  g_auto = true;
  light_apply(70);

  printf("LIGHT: ready, auto=%d level=%d%%\n", g_auto ? 1 : 0, g_level);
  return OK;
}

/****************************************************************************
 * Name: ai_watch_light_start
 *
 * Description:
 *   Spin up the ambient-light sampling thread. Split out of init on purpose.
 *
 *   The LCPU bring-up right after init (lcpu patch -> cal -> release) is
 *   timing-sensitive down to the microsecond, and the HCI commands that drive
 *   it are issued from the main thread. Whenever the main thread blocks in
 *   HAL_Delay_us() it gives the CPU away, and a 1 Hz I2C reader that is
 *   already running can land inside those gaps. Nothing here is urgent --
 *   the panel keeps the boot brightness until the first conversion -- so the
 *   thread waits until the radio is up (or has given up and scheduled a
 *   retry). Call this from the same thread that finishes BLE bring-up.
 *
 ****************************************************************************/

void ai_watch_light_start(void)
{
  pthread_attr_t attr;
  struct sched_param param;

  if (!g_present || g_thread_running)
    {
      return;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, LIGHT_STACKSIZE);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  param.sched_priority = LIGHT_PRIORITY;
  pthread_attr_setschedparam(&attr, &param);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  if (pthread_create(&g_thread, &attr, light_thread, NULL) == 0)
    {
      g_thread_running = true;
    }
  else
    {
      printf("LIGHT: thread create failed\n");
    }

  pthread_attr_destroy(&attr);
}

bool ai_watch_light_present(void)
{
  return g_present;
}

uint32_t ai_watch_light_lux(void)
{
  return g_lux;
}

bool ai_watch_light_auto(void)
{
  return g_auto;
}

void ai_watch_light_set_auto(bool on)
{
  g_auto = on;
  printf("LIGHT: auto=%d\n", on ? 1 : 0);
}

void ai_watch_light_set_level(int percent)
{
  static int logged = -1;

  g_auto = false;
  g_pending_level = -1;
  light_apply(percent);

  /* Dragging the settings bar walks one percent at a time. Log the moves that
   * matter rather than every step, so the console stays readable at 1 Mbaud.
   */

  if (logged < 0 || g_level - logged >= 5 || logged - g_level >= 5)
    {
      printf("LIGHT: level=%d%% (manual)\n", g_level);
      logged = g_level;
    }
}

int ai_watch_light_level(void)
{
  return g_level;
}

void ai_watch_light_poll(void)
{
  int pending = g_pending_level;

  if (pending < 0)
    {
      return;
    }

  g_pending_level = -1;

  light_apply(pending);
  printf("LIGHT: lux=%lu -> %d%%\n", (unsigned long)g_lux, pending);
}
