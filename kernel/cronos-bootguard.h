/*
 * PROJECT:     efi-emulation
 * LICENSE:     MIT OR MPL-2.0 (https://spdx.org/licenses/MIT, https://spdx.org/licenses/MPL-2.0)
 * PURPOSE:     One-shot fastboot return for the MT8163 Cronos board
 * COPYRIGHT:   Copyright 2026 Jonathan McAvin <mcavinjohn@gmail.com>
 */

/* Cronos' one-shot fastboot return, using the MT8163 PMIC wrapper and WDT. */
#ifndef CRONOS_BOOTGUARD_H
#define CRONOS_BOOTGUARD_H

#define CRONOS_PWRAP 0x1000d000U
#define CRONOS_WDT 0x10007000U

static UINT32 cronos_mmio_read(UINTN addr) {
  return *(volatile UINT32 *)addr;
}

static void cronos_mmio_write(UINTN addr, UINT32 value) {
  *(volatile UINT32 *)addr = value;
  __asm__ volatile("dsb sy" : : : "memory");
}

static int cronos_wait_fsm(UINT32 fsm) {
  for (UINT32 i = 0; i < 1000000; ++i) {
    UINT32 v = cronos_mmio_read(CRONOS_PWRAP + 0xa4);
    if ((v & (1U << 21)) && ((v >> 16) & 7U) == fsm)
      return 0;
  }
  return -1;
}

static int cronos_pwrap_read(UINT32 addr, UINT32 *value) {
  if (cronos_wait_fsm(0)) return -1;
  cronos_mmio_write(CRONOS_PWRAP + 0xa0, (addr >> 1) << 16);
  if (cronos_wait_fsm(6)) return -1;
  *value = cronos_mmio_read(CRONOS_PWRAP + 0xa4) & 0xffffU;
  cronos_mmio_write(CRONOS_PWRAP + 0xa8, 1);
  return 0;
}

static int cronos_pwrap_write(UINT32 addr, UINT32 value) {
  if (cronos_wait_fsm(0)) return -1;
  cronos_mmio_write(CRONOS_PWRAP + 0xa0, 0x80000000U | ((addr >> 1) << 16) | value);
  return cronos_wait_fsm(0);
}

static int cronos_rtc_trigger(void) {
  UINT32 bbpu;
  if (cronos_pwrap_write(0x803c, 1)) return -1;
  for (UINT32 i = 0; i < 100000; ++i) {
    if (cronos_pwrap_read(0x8000, &bbpu)) return -1;
    if (!(bbpu & (1U << 6))) return 0;
  }
  return -1;
}

static int cronos_bootguard_arm(void) {
  UINT32 pdn1;
  if (cronos_pwrap_write(0x8036, 0x586a) || cronos_rtc_trigger()) return -1;
  if (cronos_pwrap_write(0x8036, 0x9136) || cronos_rtc_trigger()) return -1;
  if (cronos_pwrap_read(0x802c, &pdn1)) return -1;
  if (cronos_pwrap_write(0x802c, pdn1 | (1U << 13)) || cronos_rtc_trigger()) return -1;
  if (cronos_pwrap_read(0x802c, &pdn1) || !(pdn1 & (1U << 13))) return -1;

  UINT32 mode = cronos_mmio_read(CRONOS_WDT);
  mode &= ~(1U | 8U | 64U);
  mode |= 0x22000000U | 4U | 16U;
  cronos_mmio_write(CRONOS_WDT, mode);
  cronos_mmio_write(CRONOS_WDT + 4, 50U * 2048U | 8U);
  cronos_mmio_write(CRONOS_WDT + 8, 0x1971U);
  cronos_mmio_write(CRONOS_WDT, mode | 1U);
  return 0;
}

#endif
