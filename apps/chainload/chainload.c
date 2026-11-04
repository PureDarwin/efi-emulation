/*
 * PROJECT:     efi-emulation
 * LICENSE:     MIT OR MPL-2.0 (https://spdx.org/licenses/MIT, https://spdx.org/licenses/MPL-2.0)
 * PURPOSE:     Maps a PE32+ EFI application from the module volume and starts it
 * COPYRIGHT:   Copyright 2026 Jonathan McAvin <mcavinjohn@gmail.com>
 */

#include "efi_emulation.h"
#include <efilib.h>

/* Stand-in for the xnu loader app: maps a PE32+ EFI application such as the
 * ReactOS uefildr.efi from the module volume, relocates it and starts it with
 * the emulated system table. The disk becomes its boot device so a loader that
 * reads partitions itself finds the install there. */

#define CHAINLOAD_PATH L"\\EFI\\BOOT\\uefildr.efi"

typedef struct {
  UINT16 e_magic;
  UINT16 e_unused[29];
  UINT32 e_lfanew;
} DosHeader;

typedef struct {
  UINT32 Signature;
  UINT16 Machine;
  UINT16 NumberOfSections;
  UINT32 TimeDateStamp;
  UINT32 PointerToSymbolTable;
  UINT32 NumberOfSymbols;
  UINT16 SizeOfOptionalHeader;
  UINT16 Characteristics;
  UINT16 Magic;
  UINT8 MajorLinkerVersion;
  UINT8 MinorLinkerVersion;
  UINT32 SizeOfCode;
  UINT32 SizeOfInitializedData;
  UINT32 SizeOfUninitializedData;
  UINT32 AddressOfEntryPoint;
  UINT32 BaseOfCode;
  UINT64 ImageBase;
  UINT32 SectionAlignment;
  UINT32 FileAlignment;
  UINT16 OsVersion[2];
  UINT16 ImageVersion[2];
  UINT16 SubsystemVersion[2];
  UINT32 Win32VersionValue;
  UINT32 SizeOfImage;
  UINT32 SizeOfHeaders;
  UINT32 CheckSum;
  UINT16 Subsystem;
  UINT16 DllCharacteristics;
  UINT64 Stack[2];
  UINT64 Heap[2];
  UINT32 LoaderFlags;
  UINT32 NumberOfRvaAndSizes;
  struct {
    UINT32 VirtualAddress;
    UINT32 Size;
  } DataDirectory[16];
} __attribute__((packed)) Pe64Headers;

typedef struct {
  CHAR8 Name[8];
  UINT32 VirtualSize;
  UINT32 VirtualAddress;
  UINT32 SizeOfRawData;
  UINT32 PointerToRawData;
  UINT32 Reloc[2];
  UINT16 Counts[2];
  UINT32 Characteristics;
} SectionHeader;

#define PE_MAGIC_64 0x20b
#define DIR_BASERELOC 5

typedef EFI_STATUS(EFIAPI *EfiEntry)(EFI_HANDLE, EFI_SYSTEM_TABLE *);

/* real UEFI starts applications with SSE enabled, x64 code assumes it, the
 * emulation itself runs without, so turn it on just for the child */
static void enable_sse(void) {
#if defined(__x86_64__)
  UINT64 cr0, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 = (cr0 & ~(1ULL << 2)) | (1ULL << 1); /* clear EM, set MP */
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (1ULL << 9) | (1ULL << 10); /* OSFXSR, OSXMMEXCPT */
  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
  __asm__ volatile("fninit");
#endif
}

static void say(EFI_SYSTEM_TABLE *st, CHAR16 *s) {
  st->ConOut->OutputString(st->ConOut, s);
}

static EFI_STATUS read_image(EFI_SYSTEM_TABLE *st, EFI_HANDLE device, VOID **data,
                             UINTN *size) {
  EFI_GUID fs_guid = SIMPLE_FILE_SYSTEM_PROTOCOL;
  EFI_GUID info_guid = EFI_FILE_INFO_ID;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_FILE_PROTOCOL *root, *file;
  EFI_STATUS s = st->BootServices->HandleProtocol(device, &fs_guid, (VOID **)&fs);
  if (EFI_ERROR(s))
    return s;
  s = fs->OpenVolume(fs, &root);
  if (EFI_ERROR(s))
    return s;
  s = root->Open(root, &file, CHAINLOAD_PATH, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(s))
    return s;
  UINT8 info_buf[sizeof(EFI_FILE_INFO) + 256];
  UINTN info_size = sizeof(info_buf);
  s = file->GetInfo(file, &info_guid, &info_size, info_buf);
  if (EFI_ERROR(s))
    return s;
  *size = (UINTN)((EFI_FILE_INFO *)info_buf)->FileSize;
  s = st->BootServices->AllocatePool(EfiLoaderData, *size, data);
  if (EFI_ERROR(s))
    return s;
  UINTN got = *size;
  s = file->Read(file, &got, *data);
  file->Close(file);
  return (!EFI_ERROR(s) && got != *size) ? EFI_LOAD_ERROR : s;
}

static EFI_STATUS map_image(EFI_SYSTEM_TABLE *st, UINT8 *file, UINTN size, UINT8 **base_out,
                            UINTN *image_size, EfiEntry *entry) {
  DosHeader *dos = (DosHeader *)file;
  if (size < sizeof(*dos) || dos->e_magic != 0x5a4d || dos->e_lfanew + sizeof(Pe64Headers) > size)
    return EFI_LOAD_ERROR;
  Pe64Headers *nt = (Pe64Headers *)(file + dos->e_lfanew);
  if (nt->Signature != 0x4550 || nt->Magic != PE_MAGIC_64)
    return EFI_UNSUPPORTED;
  EFI_PHYSICAL_ADDRESS base = 0;
  UINTN pages = (nt->SizeOfImage + 0xfff) / 0x1000;
  EFI_STATUS s = st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderCode, pages, &base);
  if (EFI_ERROR(s))
    return s;
  UINT8 *image = (UINT8 *)(UINTN)base;
  SetMem(image, pages * 0x1000, 0);
  CopyMem(image, file, nt->SizeOfHeaders < size ? nt->SizeOfHeaders : size);

  SectionHeader *sec = (SectionHeader *)((UINT8 *)&nt->Magic + nt->SizeOfOptionalHeader);
  for (UINTN i = 0; i < nt->NumberOfSections; ++i) {
    UINT32 raw = sec[i].SizeOfRawData;
    if (sec[i].VirtualSize && raw > sec[i].VirtualSize)
      raw = sec[i].VirtualSize;
    if (sec[i].PointerToRawData + (UINTN)raw > size ||
        sec[i].VirtualAddress + (UINTN)raw > nt->SizeOfImage)
      return EFI_LOAD_ERROR;
    CopyMem(image + sec[i].VirtualAddress, file + sec[i].PointerToRawData, raw);
  }

  /* base relocations, the image is linked low and loaded wherever pages were free */
  UINT64 delta = (UINT64)(UINTN)image - nt->ImageBase;
  if (delta && nt->NumberOfRvaAndSizes > DIR_BASERELOC) {
    UINT32 rva = nt->DataDirectory[DIR_BASERELOC].VirtualAddress;
    UINT32 left = nt->DataDirectory[DIR_BASERELOC].Size;
    while (left >= 8) {
      UINT32 page = *(UINT32 *)(image + rva);
      UINT32 block = *(UINT32 *)(image + rva + 4);
      if (block < 8 || block > left)
        return EFI_LOAD_ERROR;
      UINT16 *entries = (UINT16 *)(image + rva + 8);
      for (UINTN i = 0; i < (block - 8) / 2; ++i) {
        UINT8 *at = image + page + (entries[i] & 0xfff);
        switch (entries[i] >> 12) {
        case 0: /* padding */
          break;
        case 3: /* 32 bit */
          *(UINT32 *)at += (UINT32)delta;
          break;
        case 10: /* 64 bit */
          *(UINT64 *)at += delta;
          break;
        default:
          return EFI_UNSUPPORTED;
        }
      }
      rva += block;
      left -= block;
    }
  }
  *base_out = image;
  *image_size = nt->SizeOfImage;
  *entry = (EfiEntry)(image + nt->AddressOfEntryPoint);
  return EFI_SUCCESS;
}

/* called directly by firmware.c, so the plain C convention like src/main.c */
EFI_STATUS efi_main(EFI_HANDLE handle, EFI_SYSTEM_TABLE *st) {
  EFI_GUID loaded_guid = LOADED_IMAGE_PROTOCOL;
  EFI_LOADED_IMAGE *loaded;
  EFI_STATUS s = st->BootServices->HandleProtocol(handle, &loaded_guid, (VOID **)&loaded);
  if (EFI_ERROR(s)) {
    say(st, L"chainload: no loaded image protocol\r\n");
    return s;
  }
  VOID *file;
  UINTN size;
  s = read_image(st, loaded->DeviceHandle, &file, &size);
  if (EFI_ERROR(s)) {
    say(st, L"chainload: cannot read " CHAINLOAD_PATH L"\r\n");
    return s;
  }
  UINT8 *image;
  UINTN image_size;
  EfiEntry entry;
  s = map_image(st, file, size, &image, &image_size, &entry);
  if (EFI_ERROR(s)) {
    say(st, L"chainload: not a loadable PE32+ EFI image\r\n");
    return s;
  }
  st->BootServices->FreePool(file);

  /* the child sees itself through the same handle, its boot device is the disk */
  loaded->ImageBase = image;
  loaded->ImageSize = image_size;
  loaded->ImageCodeType = EfiLoaderCode;
  loaded->ImageDataType = EfiLoaderData;
  VOID *block_io;
  EFI_GUID block_guid = BLOCK_IO_PROTOCOL;
  /* boot from the active partition like firmware would after reading its MBR,
   * the whole disk only when there is no partition table */
  if (efiemu_disk_boot_partition())
    loaded->DeviceHandle = efiemu_disk_boot_partition();
  else if (!EFI_ERROR(efiemu_disk_protocol(&block_guid, &block_io)))
    loaded->DeviceHandle = efiemu_disk_handle();
  say(st, L"chainload: starting " CHAINLOAD_PATH L"\r\n");
  enable_sse();
  return entry(handle, st);
}
