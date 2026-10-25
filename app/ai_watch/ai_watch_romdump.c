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
#include <stdint.h>
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/arch.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ROMDUMP_ROM_BASE         0x20800000UL
#define ROMDUMP_ROM_SIZE         (384 * 1024)
#define ROMDUMP_LPS_BASE         0x20400000UL
#define ROMDUMP_LPS_SIZE         (64 * 1024)
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

  static const struct romdump_region_s regions[2] =
    {
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

  syslog(LOG_INFO, "ROMDUMP: fault-time LPS snapshot, watch is frozen\n");

  for (i = 0; i < 2; i++)
    {
      romdump_region(fd, &regions[i]);
    }

  close(fd);
  syslog(LOG_INFO, "ROMDUMP: LPS snapshot complete\n");
}

#else /* CONFIG_AI_WATCH_ROMDUMP */

typedef int romdump_unused_t;

#endif /* CONFIG_AI_WATCH_ROMDUMP */
