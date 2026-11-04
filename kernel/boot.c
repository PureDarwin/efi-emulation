/*
 * PROJECT:     efi-emulation
 * LICENSE:     MIT OR MPL-2.0 (https://spdx.org/licenses/MIT, https://spdx.org/licenses/MPL-2.0)
 * PURPOSE:     Common boot protocol glue
 * COPYRIGHT:   Copyright 2026 Jonathan McAvin <mcavinjohn@gmail.com>
 */

#include "efi_emulation.h"
#include "serial.h"

#define MB2_BOOTLOADER_MAGIC 0x36d76289ULL
#define KERNEL_LINUX_MAGIC 0x584e554cULL

void kernel_multiboot2_main(UINT64 magic, UINT64 mbi);
void kernel_linux_main(UINT64 boot_params);

/* Every entry stub lands here in long mode with its protocol's magic. */
void kernel_boot_main(UINT64 magic, UINT64 info) {
  serial_reinit();
  efiemu_exceptions_install();
  if (magic == MB2_BOOTLOADER_MAGIC)
    kernel_multiboot2_main(magic, info);
  else if (magic == KERNEL_LINUX_MAGIC)
    kernel_linux_main(info);
  efiemu_debug_string("efi-emulation: unknown boot protocol\n");
  for (;;)
    __asm__ volatile("cli; hlt");
}
