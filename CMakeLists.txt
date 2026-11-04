# efi-emulation: EFI boot and runtime services on top of the Linux or multiboot2
# boot protocol, so an EFI application boots as a plain kernel image
cmake_minimum_required(VERSION 3.20)
project(efi-emulation C ASM)

# firmware code, the per arch files carry the exact flags xnu-loader always used
foreach(lang C ASM ASM_NASM)
    foreach(config RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
        set(CMAKE_${lang}_FLAGS_${config} "")
    endforeach()
endforeach()

set(GNU_EFI_ROOT "" CACHE PATH "gnu-efi install prefix with include/efi and lib")
set(EFIEMU_APP "chainload" CACHE STRING "built-in EFI application, chainload loads \\EFI\\BOOT from the initrd")
set(EFIEMU_EMBED_INITRD "" CACHE FILEPATH "cpio archive linked into the image instead of the loader's initrd")
set(EFIEMU_PLATFORM "qemuvirt" CACHE STRING "arm64 board: generic, qemuvirt, sun50i, bcm2837 or sg2002")
set(EFIEMU_EXTRA_DEFINES "" CACHE STRING "extra -D flags for board bring-up, see kernel/arm64.c")

if(NOT GNU_EFI_ROOT)
    message(FATAL_ERROR "set GNU_EFI_ROOT to a gnu-efi built for the target")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(EFIEMU_ARCH x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(EFIEMU_ARCH aarch64)
else()
    message(FATAL_ERROR "unsupported target ${CMAKE_SYSTEM_PROCESSOR}, x86_64 and aarch64 build so far")
endif()

include(cmake/${EFIEMU_ARCH}.cmake)
