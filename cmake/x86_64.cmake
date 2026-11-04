# x86_64: a bzImage for the Linux boot protocol, also a multiboot2 ELF
enable_language(ASM_NASM)

set(EFIEMU_GNU_EFI_INCLUDES
    ${GNU_EFI_ROOT}/include
    ${GNU_EFI_ROOT}/include/efi
    ${GNU_EFI_ROOT}/include/efi/x86_64
    ${GNU_EFI_ROOT}/include/efi/protocol)

set(EFIEMU_COMMON_FLAGS
    -m64 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
    -mgeneral-regs-only -fshort-wchar)

# entry stubs and the exception vectors
add_library(efiemu_entry OBJECT
    kernel/entry/multiboot2.S
    kernel/entry/linux.S
    src/exceptions.S
    kernel/entry/embedded.S)
target_compile_options(efiemu_entry PRIVATE -m64 -ffreestanding -fno-pic)
if(EFIEMU_EMBED_INITRD)
    set_source_files_properties(kernel/entry/embedded.S PROPERTIES
        COMPILE_DEFINITIONS "EMBED_CPIO=\"${EFIEMU_EMBED_INITRD}\""
        OBJECT_DEPENDS ${EFIEMU_EMBED_INITRD})
endif()

# boot protocol glue and the emulated firmware, ms abi like real UEFI
add_library(efiemu OBJECT
    kernel/boot.c
    kernel/multiboot2.c
    kernel/linux.c
    kernel/cpio.c
    src/exceptions.c
    src/firmware.c
    src/modfs.c
    src/storage.c
    src/input.c)
target_compile_options(efiemu PRIVATE ${EFIEMU_COMMON_FLAGS})
target_compile_definitions(efiemu PRIVATE GNU_EFI_USE_MS_ABI)
target_include_directories(efiemu PRIVATE src include ${EFIEMU_GNU_EFI_INCLUDES})

# the application efiemu_main hands control to through efi_main
if(EFIEMU_APP STREQUAL "chainload")
    set(EFIEMU_APP_SOURCES apps/chainload/chainload.c board/serial.c)
else()
    message(FATAL_ERROR "unknown EFIEMU_APP ${EFIEMU_APP}")
endif()
add_library(efiemu_app OBJECT ${EFIEMU_APP_SOURCES})
target_compile_options(efiemu_app PRIVATE ${EFIEMU_COMMON_FLAGS}
    -maccumulate-outgoing-args -mno-avx -funsigned-char)
target_compile_definitions(efiemu_app PRIVATE GNU_EFI_USE_MS_ABI LEGACY_BIOS CONFIG_x86_64)
target_include_directories(efiemu_app PRIVATE src include ${EFIEMU_GNU_EFI_INCLUDES})

execute_process(COMMAND ${CMAKE_C_COMPILER} -m64 -print-libgcc-file-name
    OUTPUT_VARIABLE EFIEMU_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)

set(EFIEMU_ELF ${CMAKE_BINARY_DIR}/efi-emulation.elf)
set(EFIEMU_BZIMAGE ${CMAKE_BINARY_DIR}/efi-emulation.bzImage)

add_custom_command(OUTPUT ${EFIEMU_ELF}
    COMMAND ${CMAKE_LINKER} -nostdlib --allow-multiple-definition -z max-page-size=0x1000
        -T ${CMAKE_SOURCE_DIR}/kernel/linker.ld -o ${EFIEMU_ELF}
        $<TARGET_OBJECTS:efiemu_entry> $<TARGET_OBJECTS:efiemu> $<TARGET_OBJECTS:efiemu_app>
        -L${GNU_EFI_ROOT}/lib -lgnuefi -lefi ${EFIEMU_LIBGCC}
    DEPENDS efiemu_entry efiemu efiemu_app
        $<TARGET_OBJECTS:efiemu_entry> $<TARGET_OBJECTS:efiemu> $<TARGET_OBJECTS:efiemu_app>
        ${CMAKE_SOURCE_DIR}/kernel/linker.ld
    COMMAND_EXPAND_LISTS
    VERBATIM)

# setup sectors from nasm in front of the flat payload starting at __kernel_start
add_custom_command(OUTPUT ${EFIEMU_BZIMAGE}
    COMMAND ${CMAKE_COMMAND}
        -DELF=${EFIEMU_ELF} -DOUT=${EFIEMU_BZIMAGE}
        -DOBJCOPY=${CMAKE_OBJCOPY} -DNM=${CMAKE_NM} -DNASM=${CMAKE_ASM_NASM_COMPILER}
        -DSETUP=${CMAKE_SOURCE_DIR}/kernel/entry/linux-setup.asm
        -P ${CMAKE_SOURCE_DIR}/cmake/bzimage.cmake
    DEPENDS ${EFIEMU_ELF} ${CMAKE_SOURCE_DIR}/kernel/entry/linux-setup.asm
        ${CMAKE_SOURCE_DIR}/cmake/bzimage.cmake
    VERBATIM)

add_custom_target(efiemu_image ALL DEPENDS ${EFIEMU_BZIMAGE})
install(FILES ${EFIEMU_ELF} ${EFIEMU_BZIMAGE} DESTINATION boot)
