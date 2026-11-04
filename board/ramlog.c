/*
 * PROJECT:     efi-emulation
 * LICENSE:     MIT OR MPL-2.0 (https://spdx.org/licenses/MIT, https://spdx.org/licenses/MPL-2.0)
 * PURPOSE:     DRAM console that survives a warm reset
 * COPYRIGHT:   Copyright 2026 Jonathan McAvin <mcavinjohn@gmail.com>
 */

/* The DRAM console described in ramlog.h. Runs before and after the MMU is on,
 * so it is built with -mstrict-align, and every store is cleaned to the point
 * of coherency at once: a reset does not write back dirty cache lines. */
#include "ramlog.h"

#if defined(XNU_LOADER_RAMLOG)

static BOOLEAN ramlog_ready;
// the head is kept here and the ring takes whole aligned words only:
// tegra210 DRAM with the MMU off gave back a corrupted head after byte stores
static UINT32 ramlog_head, ramlog_word;

static volatile UINT32 *copy_base(UINT32 i) {
  return (volatile UINT32 *)(UINTN)(XNU_LOADER_RAMLOG + (UINT64)i * RAMLOG_COPY_SIZE);
}

static VOID clean(volatile VOID *p) {
  __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
}

VOID ramlog_init(VOID) {
  /* the boot count survives only as well as DRAM does: a hint, not a record */
  UINT32 boots = copy_base(0)[0] == RAMLOG_MAGIC ? copy_base(0)[3] + 1 : 1;

  for (UINT32 i = 0; i < RAMLOG_COPIES; ++i) {
    volatile UINT32 *h = copy_base(i);
    h[0] = RAMLOG_MAGIC;
    h[1] = 0;
    h[2] = RAMLOG_COPY_SIZE - RAMLOG_HEADER;
    h[3] = boots;
    clean(h);
  }
  ramlog_head = 0;
  __asm__ volatile("dsb sy" : : : "memory");
  ramlog_ready = TRUE;
}

VOID ramlog_mark(CHAR8 c) {
  for (UINT32 i = 0; i < RAMLOG_COPIES; ++i) {
    volatile UINT32 *m = (volatile UINT32 *)(UINTN)(XNU_LOADER_RAMLOG + RAMLOG_MARKS_OFFSET +
                                                    (UINT64)i * RAMLOG_MARK_STRIDE);
    *m = (UINT8)c;
    clean(m);
  }
  __asm__ volatile("dsb sy" : : : "memory");
#if defined(XNU_LOADER_MARK_LED)
  led_mark(c);
#endif
}

#if defined(XNU_LOADER_MARK_LED)
/* uz801 leds (tlmm gpio, active high): red 72, green 71, blue 73. bit 0 red, 1 green, 2 blue */
VOID led_mark(CHAR8 c) {
  static CONST CHAR8 order[] = "P1BRCFMIEX";
  static CONST UINT8 colour[] = { 1, 2, 4, 3, 5, 6, 7, 1, 2, 4 };
  UINT8 bits = 7;
  for (UINT32 i = 0; order[i] != 0; ++i)
    if (order[i] == c)
      bits = colour[i];
  ((volatile UINT32 *)(UINTN)0x01048004)[0] = (bits & 1) ? 2 : 0;
  ((volatile UINT32 *)(UINTN)0x01047004)[0] = (bits & 2) ? 2 : 0;
  ((volatile UINT32 *)(UINTN)0x01049004)[0] = (bits & 4) ? 2 : 0;
  __asm__ volatile("dsb sy" : : : "memory");
  /* hold each colour for 0.4 s of the 19.2 mhz counter so a camera sees the sequence */
  UINT64 t0, t;
  __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0));
  do
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
  while (t - t0 < 7680000);
}
#endif

VOID ramlog_putc(CHAR8 c) {
  if (!ramlog_ready)
    return;
  UINT32 off = ramlog_head % (RAMLOG_COPY_SIZE - RAMLOG_HEADER);
  if ((off & 3) == 0)
    ramlog_word = 0;
  ramlog_word |= (UINT32)(UINT8)c << ((off & 3) * 8);
  ++ramlog_head;
  for (UINT32 i = 0; i < RAMLOG_COPIES; ++i) {
    volatile UINT32 *h = copy_base(i);
    volatile UINT32 *slot = h + (RAMLOG_HEADER + (off & ~3U)) / 4;
    *slot = ramlog_word;
    clean(slot);
    h[1] = ramlog_head;
    clean(h);
  }
  __asm__ volatile("dsb sy" : : : "memory");
}

#endif
