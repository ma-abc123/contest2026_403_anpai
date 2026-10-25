/****************************************************************************
 * apps/ai_watch/ai_watch_romdump.c
 *
 * One-shot LCPU firmware dumper for offline reverse engineering
 * (CONFIG_AI_WATCH_ROMDUMP, diagnostic builds only).
 *
 * The closed-source Bluetooth controller firmware executes in the LCPU
 * mask ROM; the boot stub's vector table (g_lcpu_bin) points its reset
 * and interrupt handlers into the 0x00400000 code-bus window.  This file
 * hex-dumps two windows to the console:
 *
 *   ROM 0x20800000, 384 KiB - LCPU ROM (controller firmware image;
 *                             HCPU view via LCPU_ROM_ADDR_2_HCPU_ADDR,
 *                             LCPU-local base 0x00000000 + 0x20800000)
 *   LPS 0x20400000,  64 KiB - LPSYS shared window (LCPU RAM, patch area,
 *                             EM incl. the assert-info record, ROM_RAM)
 *
 * Each line covers 16 bytes; BEGIN/END markers carry a CRC32 per region
 * so the host-side parser can verify completeness.  Cache handling
 * follows sf32lb52_lcpu_boot.c: invalidate before reading LCPU-written
 * windows (the HCPU only ever reads these regions, and the bridge layer
 * keeps its own writes clean, so no dirty lines are lost).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_AI_WATCH_ROMDUMP

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/arch.h>

#include "ai_watch_ble_bsp.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ROMDUMP_ROM_BASE         0x20800000UL
#define ROMDUMP_ROM_SIZE         (384 * 1024)
#define ROMDUMP_LPS_BASE         0x20400000UL
#define ROMDUMP_LPS_SIZE         (64 * 1024)
/* HPSYS mailbox windows, HCPU side of the HCI transport:
 * 0x2007FC00 = HCPU2LCPU_CH2 (rev-B config context), 0x2007FE00 =
 * HCPU2LCPU_CH1 (command ring: circular_buf, rd mirror at +0x08 is
 * LCPU-written).  r7's wedge question "did the LCPU ever see the
 * pending command" can only be answered from this window - the LPS
 * dump does not cover HPSYS RAM. */
#define ROMDUMP_H2L_BASE         0x2007FC00UL
#define ROMDUMP_H2L_SIZE         0x400
#define ROMDUMP_CHUNK            16
#define ROMDUMP_CRC32_POLY       0xedb88320UL

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct romdump_region_s
{
  const char *tag;
  uintptr_t base;
  size_t size;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t romdump_crc32(FAR const uint8_t *p, size_t len,
                              uint32_t crc)
{
  size_t i;
  int bit;

  for (i = 0; i < len; i++)
    {
      crc ^= p[i];
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc >> 1) ^ (ROMDUMP_CRC32_POLY & (0UL - (crc & 1)));
        }
    }

  return crc;
}

static void romdump_region(int fd, const struct romdump_region_s *r)
{
  uint32_t crc;
  uintptr_t addr;
  size_t off;

  syslog(LOG_INFO, "ROMDUMP BEGIN region=%s base=0x%08lx size=0x%08zx\n",
         r->tag, (unsigned long)r->base, r->size);

  crc = 0xffffffffUL;
  for (off = 0; off < r->size; off += ROMDUMP_CHUNK)
    {
      volatile uint32_t *w = (volatile uint32_t *)(r->base + off);
      char line[64];
      int len;
      int i;

      /* Pace the flood: a short gap every 64 lines lets the USB-serial
       * receiver drain its small FIFO without overruns (1 Mbaud, no
       * flow control on this console). */

      if ((off / ROMDUMP_CHUNK) % 64 == 63)
        {
          usleep(20000);
        }

      up_invalidate_dcache(r->base + off, r->base + off + 2 *
                           ROMDUMP_CHUNK);

      for (i = 0; i < ROMDUMP_CHUNK / 4; i++)
        {
          crc = romdump_crc32((FAR const uint8_t *)&w[i], 4, crc);
        }

      len = snprintf(line, sizeof(line), "ROMD %08lx: %08x %08x %08x %08x\n",
                     (unsigned long)(r->base + off),
                     (unsigned)w[0], (unsigned)w[1],
                     (unsigned)w[2], (unsigned)w[3]);
      write(fd, line, len);
    }

  syslog(LOG_INFO, "ROMDUMP END region=%s crc=0x%08lx\n",
         r->tag, (unsigned long)(crc ^ 0xffffffffUL));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ai_watch_romdump_run(void)
{
  /* Each region is dumped twice: the console is shared with other
   * syslog producers (BT stack warnings), which can garble individual
   * lines mid-write.  The two passes interleave differently, so the
   * host-side parser picks whichever pass verifies against its CRC32
   * (and can merge per-line if both are partially damaged). */

  static const struct romdump_region_s regions[4] =
    {
      { "ROM_A", ROMDUMP_ROM_BASE, ROMDUMP_ROM_SIZE },
      { "ROM_B", ROMDUMP_ROM_BASE, ROMDUMP_ROM_SIZE },
      { "LPS_A", ROMDUMP_LPS_BASE, ROMDUMP_LPS_SIZE },
      { "LPS_B", ROMDUMP_LPS_BASE, ROMDUMP_LPS_SIZE }
    };

  int fd;
  int i;

  fd = open("/dev/console", O_WRONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "ROMDUMP: cannot open console\n");
      return;
    }

  syslog(LOG_INFO, "ROMDUMP: dumping 2x%zu KiB, watch is frozen meanwhile\n",
         (regions[0].size + regions[2].size) / 1024);

  for (i = 0; i < 4; i++)
    {
      romdump_region(fd, &regions[i]);
    }

  close(fd);
  syslog(LOG_INFO, "ROMDUMP: complete\n");
}

void ai_watch_romdump_run_lps(void)
{
  /* Fault-time snapshot of the LPSYS window only (assert record at
   * 0x20407FE0, patch hooks, cfg): called from the BLE fault path
   * BEFORE the recovery tears the LCPU down, so the on-fault state is
   * preserved for offline analysis. */

  static const struct romdump_region_s regions[4] =
    {
      { "LPS_A", ROMDUMP_LPS_BASE, ROMDUMP_LPS_SIZE },
      { "LPS_B", ROMDUMP_LPS_BASE, ROMDUMP_LPS_SIZE },
      { "H2L_A", ROMDUMP_H2L_BASE, ROMDUMP_H2L_SIZE },
      { "H2L_B", ROMDUMP_H2L_BASE, ROMDUMP_H2L_SIZE }
    };

  int fd;
  int i;

  fd = open("/dev/console", O_WRONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "ROMDUMP: cannot open console\n");
      return;
    }

  syslog(LOG_INFO, "ROMDUMP: fault-time LPS snapshot, watch is frozen\n");

  for (i = 0; i < 4; i++)
    {
      romdump_region(fd, &regions[i]);
    }

  close(fd);
  syslog(LOG_INFO, "ROMDUMP: LPS snapshot complete\n");
}

void ai_watch_romdump_run_regs(void)
{
  /* Fault-time snapshot of the LPSYS Bluetooth register blocks.  Only
   * RCC/RFC/PHY are readable from the HCPU: BT_MAC (0x40090000) hard-
   * faults on ANY HCPU access, healthy or faulted (verified both ways
   * 2026-10-25 - the earlier "MAC reads all zero" was the parser's
   * zero-fill for a region whose lines never made it out).  MAC is
   * LCPU-private; SWD remains the only way to see it. */

  /* MAC (0x40090000) intentionally NOT read here.  r6 request was to
   * capture it at bring-up failures, but the c2e1 build proved the
   * abort is context-dependent: it hard-faulted at a 0x2036 bring-up
   * fault while r4-era connection faults read it fine (zeros) and the
   * healthy sampler reads it every 100 ms.  A bus abort here hangs
   * the board before recovery; MAC evidence at bring-up faults has to
   * come from the SWD stage instead. */

  static const struct romdump_region_s regs[3] =
    {
      { "RCC", 0x40000000UL, 0x40 },
      { "RFC", 0x40082800UL, 0x40 },
      { "PHY", 0x40084000UL, 0x40 }
    };

  int fd;
  int i;

  fd = open("/dev/console", O_WRONLY);
  if (fd < 0)
    {
      syslog(LOG_ERR, "ROMDUMP: cannot open console\n");
      return;
    }

  syslog(LOG_INFO, "ROMDUMP: fault-time LPSYS register snapshot\n");

  for (i = 0; i < 3; i++)
    {
      romdump_region(fd, &regs[i]);
    }

  close(fd);
  syslog(LOG_INFO, "ROMDUMP: register snapshot complete\n");
}

/****************************************************************************
 * r5 register sampler (H1/H2/H3, analyst-provided design, NuttX port)
 *
 * 100 ms samples of the LPSYS Bluetooth register blocks + HCI ring
 * offsets + assert flag into a 32-entry RAM ring; 1 Hz REGLIVE line so
 * the timeline survives even a later crash.  REGRING (whole ring) at
 * fault latch, REGFULL (full blocks) at BLE ready / connected / fault.
 *
 * Sampling is GATED on the bridge state: only ADVERTISING / CONNECTED
 * / DISCONNECTED are sampled.  During recover() the LCPU is halted and
 * at bt_enable failure windows the controller state is undefined -
 * reading BT_MAC there has hard-faulted the HCPU before (reg2 era),
 * and a userspace thread cannot survive a bus abort.
 *
 ****************************************************************************/

#define SAMP_RING_N      32
#define SAMP_MS          100
#define SAMP_RCC_BASE    0x40000000UL
#define SAMP_MAC_BASE    0x40090000UL
#define SAMP_RFC_BASE    0x40082800UL
#define SAMP_PHY_BASE    0x40084000UL
#define SAMP_HCI_RD      0x20405C08UL
#define SAMP_HCI_WR      0x20405C0CUL
#define SAMP_ASSERT      0x20407FE0UL
/* HCPU2LCPU_CH1 command ring mirrors (see ROMDUMP_H2L_BASE): +0x08 is
 * the LCPU-consumed read index, +0x0C the HCPU-written write index.
 * Read them passively: the bridge invalidates this window before every
 * TX-ring read and cleans it after every write, so plain volatile
 * reads are fresh enough.  Do NOT invalidate here - an invalidate
 * racing the bridge's ring init drops the init's still-dirty header
 * line and the LCPU then reads garbage pointers (c2e1's 0x0C03 x3). */
#define SAMP_H2L_RD      0x2007FE08UL
#define SAMP_H2L_WR      0x2007FE0CUL

void ai_watch_reg_sampler_dump_ring(const char *tag);
void ai_watch_reg_sampler_dump_full(const char *tag);

struct samp_entry
{
  uint32_t t;
  uint32_t rcc0, rcc4, rcc10, rcc14;
  uint32_t mac0, mac18, mac20;
  uint32_t rfc8, phy0;
  uint32_t rd, wr;
  uint32_t h2l_rd, h2l_wr;
  uint32_t flag;
};

static struct samp_entry s_samp_ring[SAMP_RING_N];
static int s_samp_head;
static int s_samp_count;
static volatile bool s_samp_run;
static int s_healthy_pending;          /* dumps owed since bt_ready */
static uint32_t s_last_healthy_ms;

static uint32_t samp_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static uint32_t samp_rd32(uint32_t addr)
{
  return *(volatile uint32_t *)addr;
}

static bool samp_state_ok(void)
{
  enum ai_watch_ble_bsp_state_e st = ai_watch_ble_bsp_get_state();

  return st == AI_WATCH_BLE_BSP_ADVERTISING ||
         st == AI_WATCH_BLE_BSP_CONNECTED ||
         st == AI_WATCH_BLE_BSP_DISCONNECTED;
}

static void samp_take(struct samp_entry *e)
{
  e->t     = samp_now_ms();
  e->rcc0  = samp_rd32(SAMP_RCC_BASE + 0x00);
  e->rcc4  = samp_rd32(SAMP_RCC_BASE + 0x04);
  e->rcc10 = samp_rd32(SAMP_RCC_BASE + 0x10);
  e->rcc14 = samp_rd32(SAMP_RCC_BASE + 0x14);
  e->mac0  = samp_rd32(SAMP_MAC_BASE + 0x00);
  e->mac18 = samp_rd32(SAMP_MAC_BASE + 0x18);
  e->mac20 = samp_rd32(SAMP_MAC_BASE + 0x20);
  e->rfc8  = samp_rd32(SAMP_RFC_BASE + 0x08);
  e->phy0  = samp_rd32(SAMP_PHY_BASE + 0x00);
  e->rd    = samp_rd32(SAMP_HCI_RD);
  e->wr    = samp_rd32(SAMP_HCI_WR);
  e->h2l_rd = samp_rd32(SAMP_H2L_RD);
  e->h2l_wr = samp_rd32(SAMP_H2L_WR);
  e->flag  = samp_rd32(SAMP_ASSERT);
}

static FAR void *samp_thread(FAR void *arg)
{
  int ticks = 0;

  (void)arg;

  while (s_samp_run)
    {
      if (samp_state_ok())
        {
          struct samp_entry *e = &s_samp_ring[s_samp_head];

          samp_take(e);
          s_samp_head = (s_samp_head + 1) % SAMP_RING_N;
          if (s_samp_count < SAMP_RING_N)
            {
              s_samp_count++;
            }

          ticks++;

          if (ticks % 10 == 0)
            {
              /* 1 Hz live line: survives later crashes/hangs. */

              syslog(LOG_INFO,
                     "REGLIVE t=%u rcc0=%08x rcc4=%08x mac0=%08x "
                     "mac18=%08x rfc8=%08x rd=%08x wr=%08x "
                     "h2l=%08x/%08x flag=%u\n",
                     e->t, e->rcc0, e->rcc4, e->mac0, e->mac18,
                     e->rfc8, e->rd, e->wr, e->h2l_rd, e->h2l_wr,
                     e->flag);
            }

          if (s_healthy_pending > 0 &&
              e->t - s_last_healthy_ms >= 10000u)
            {
              s_last_healthy_ms = e->t;
              s_healthy_pending--;
              ai_watch_reg_sampler_dump_full("healthy");
            }
        }

      usleep(SAMP_MS * 1000);
    }

  return NULL;
}

void ai_watch_reg_sampler_start(void)
{
  pthread_attr_t attr;
  pthread_t tid;

  if (s_samp_run)
    {
      return;
    }

  s_samp_run = true;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);
  if (pthread_create(&tid, &attr, samp_thread, NULL) != 0)
    {
      s_samp_run = false;
      syslog(LOG_ERR, "REGRING: sampler thread create failed\n");
    }
  pthread_attr_destroy(&attr);
}

void ai_watch_reg_sampler_notify_ready(void)
{
  s_healthy_pending = 3;
  s_last_healthy_ms = samp_now_ms() - 10000u;  /* dump immediately */
}

void ai_watch_reg_sampler_dump_ring(const char *tag)
{
  int start = (s_samp_head - s_samp_count + SAMP_RING_N) % SAMP_RING_N;
  int i;

  for (i = 0; i < s_samp_count; i++)
    {
      struct samp_entry *s = &s_samp_ring[(start + i) % SAMP_RING_N];

      syslog(LOG_INFO,
             "REGRING tag=%s idx=%d t=%u rcc0=%08x rcc4=%08x "
             "rcc10=%08x rcc14=%08x mac0=%08x mac18=%08x mac20=%08x "
             "rfc8=%08x phy0=%08x rd=%08x wr=%08x h2l_rd=%08x "
             "h2l_wr=%08x flag=%u\n",
             tag, i, s->t, s->rcc0, s->rcc4, s->rcc10, s->rcc14,
             s->mac0, s->mac18, s->mac20, s->rfc8, s->phy0,
             s->rd, s->wr, s->h2l_rd, s->h2l_wr, s->flag);
    }
}

void ai_watch_reg_sampler_dump_full(const char *tag)
{
  static const struct
  {
    const char *name;
    uint32_t base;
    int words;
  } blocks[4] =
    {
      { "rcc", SAMP_RCC_BASE, 8 },   /* 0x20 - no mirror */
      { "mac", SAMP_MAC_BASE, 16 },
      { "rfc", SAMP_RFC_BASE, 16 },
      { "phy", SAMP_PHY_BASE, 16 }
    };

  int b;
  int i;

  for (b = 0; b < 4; b++)
    {
      char line[16 * 9 + 1];
      int pos = 0;

      line[0] = '\0';
      for (i = 0; i < blocks[b].words; i++)
        {
          pos += snprintf(line + pos, sizeof(line) - pos, " %08x",
                          samp_rd32(blocks[b].base + 4u * i));
        }
      syslog(LOG_INFO, "REGFULL tag=%s blk=%s%s\n", tag,
             blocks[b].name, line);
    }
}

#else /* CONFIG_AI_WATCH_ROMDUMP */

typedef int romdump_unused_t;

#endif /* CONFIG_AI_WATCH_ROMDUMP */
