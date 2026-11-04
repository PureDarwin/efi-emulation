/*
 * PROJECT:     efi-emulation
 * LICENSE:     MIT OR MPL-2.0 (https://spdx.org/licenses/MIT, https://spdx.org/licenses/MPL-2.0)
 * PURPOSE:     Optional cpio archive built into the image
 * COPYRIGHT:   Copyright 2026 Jonathan McAvin <mcavinjohn@gmail.com>
 */

/* An optional cpio built into the loader, used instead of the bootloader's initrd. */
	.section .rodata.embedded_initrd, "a"
	.globl embedded_initrd_start
	.globl embedded_initrd_end
	.p2align 12
embedded_initrd_start:
#ifdef EMBED_CPIO
	.incbin EMBED_CPIO
#endif
embedded_initrd_end:
	.section .note.GNU-stack, "", @progbits
