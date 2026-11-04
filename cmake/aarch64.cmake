# aarch64: an arm64 Linux Image, for U-Boot booti, QEMU -kernel and the like
set(EFIEMU_PLATFORMS generic qemuvirt sun50i bcm2837 sg2002)
if(NOT EFIEMU_PLATFORM IN_LIST EFIEMU_PLATFORMS)
    message(FATAL_ERROR "EFIEMU_PLATFORM must be one of ${EFIEMU_PLATFORMS}")
endif()
string(TOUPPER ${EFIEMU_PLATFORM} EFIEMU_PLATFORM_UPPER)

set(EFIEMU_GNU_EFI_INCLUDES
    ${GNU_EFI_ROOT}/include
    ${GNU_EFI_ROOT}/include/efi
    ${GNU_EFI_ROOT}/include/efi/aarch64
    ${GNU_EFI_ROOT}/include/efi/protocol)

set(EFIEMU_COMMON_FLAGS
    -ffreestanding -fno-stack-protector -fno-stack-check -fpie -fshort-wchar
    -funsigned-char -O2 -ggdb -Wno-pointer-sign)

separate_arguments(EFIEMU_EXTRA_DEFINE_LIST UNIX_COMMAND "${EFIEMU_EXTRA_DEFINES}")
set(EFIEMU_DEFINES
    EFI_FUNCTION_WRAPPER CONFIG_aarch64 CONFIG_LOADER_aarch64
    XNU_LOADER_PLATFORM_${EFIEMU_PLATFORM_UPPER} LEGACY_BIOS)

add_library(efiemu_entry OBJECT
    kernel/entry/linux-arm64.S
    src/exceptions-arm64.S
    kernel/entry/embedded.S)
target_compile_options(efiemu_entry PRIVATE -fpie ${EFIEMU_EXTRA_DEFINE_LIST})
target_compile_definitions(efiemu_entry PRIVATE ${EFIEMU_DEFINES})
if(EFIEMU_EMBED_INITRD)
    set_source_files_properties(kernel/entry/embedded.S PROPERTIES
        COMPILE_DEFINITIONS "EMBED_CPIO=\"${EFIEMU_EMBED_INITRD}\""
        OBJECT_DEPENDS ${EFIEMU_EMBED_INITRD})
endif()

# board setup runs with the MMU off, where unaligned accesses fault
add_library(efiemu_board OBJECT
    kernel/arm64.c
    board/fdt_board.c
    board/ramlog.c)
target_compile_options(efiemu_board PRIVATE ${EFIEMU_COMMON_FLAGS} -mstrict-align ${EFIEMU_EXTRA_DEFINE_LIST})
target_compile_definitions(efiemu_board PRIVATE ${EFIEMU_DEFINES})
target_include_directories(efiemu_board PRIVATE src include ${EFIEMU_GNU_EFI_INCLUDES})

add_library(efiemu OBJECT
    kernel/cpio.c
    src/exceptions.c
    src/firmware.c
    src/modfs.c
    src/storage.c
    src/input.c
    board/serial.c)
target_compile_options(efiemu PRIVATE ${EFIEMU_COMMON_FLAGS} ${EFIEMU_EXTRA_DEFINE_LIST})
target_compile_definitions(efiemu PRIVATE ${EFIEMU_DEFINES})
target_include_directories(efiemu PRIVATE src include ${EFIEMU_GNU_EFI_INCLUDES})

if(EFIEMU_APP STREQUAL "chainload")
    set(EFIEMU_APP_SOURCES apps/chainload/chainload.c)
else()
    message(FATAL_ERROR "unknown EFIEMU_APP ${EFIEMU_APP}")
endif()
add_library(efiemu_app OBJECT ${EFIEMU_APP_SOURCES})
target_compile_options(efiemu_app PRIVATE ${EFIEMU_COMMON_FLAGS} ${EFIEMU_EXTRA_DEFINE_LIST})
target_compile_definitions(efiemu_app PRIVATE ${EFIEMU_DEFINES})
target_include_directories(efiemu_app PRIVATE src include ${EFIEMU_GNU_EFI_INCLUDES})

execute_process(COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name
    OUTPUT_VARIABLE EFIEMU_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)

set(EFIEMU_ELF ${CMAKE_BINARY_DIR}/efi-emulation-arm64.elf)
set(EFIEMU_IMAGE ${CMAKE_BINARY_DIR}/efi-emulation.Image)

add_custom_command(OUTPUT ${EFIEMU_ELF}
    COMMAND ${CMAKE_LINKER} -nostdlib -pie --no-dynamic-linker -z notext --no-warn-rwx-segments
        --allow-multiple-definition -z max-page-size=0x1000
        -T ${CMAKE_SOURCE_DIR}/kernel/linker-arm64.ld -o ${EFIEMU_ELF}
        $<TARGET_OBJECTS:efiemu_entry> $<TARGET_OBJECTS:efiemu_board>
        $<TARGET_OBJECTS:efiemu> $<TARGET_OBJECTS:efiemu_app>
        -L${GNU_EFI_ROOT}/lib -lefi ${EFIEMU_LIBGCC}
    DEPENDS efiemu_entry efiemu_board efiemu efiemu_app
        $<TARGET_OBJECTS:efiemu_entry> $<TARGET_OBJECTS:efiemu_board>
        $<TARGET_OBJECTS:efiemu> $<TARGET_OBJECTS:efiemu_app>
        ${CMAKE_SOURCE_DIR}/kernel/linker-arm64.ld
    COMMAND_EXPAND_LISTS
    VERBATIM)

# the entry stub only applies R_AARCH64_RELATIVE, anything else would stay unresolved
add_custom_command(OUTPUT ${EFIEMU_IMAGE}
    COMMAND ${CMAKE_COMMAND}
        -DELF=${EFIEMU_ELF} -DOUT=${EFIEMU_IMAGE}
        -DOBJCOPY=${CMAKE_OBJCOPY} -DREADELF=${CMAKE_READELF}
        -P ${CMAKE_SOURCE_DIR}/cmake/arm64-image.cmake
    DEPENDS ${EFIEMU_ELF} ${CMAKE_SOURCE_DIR}/cmake/arm64-image.cmake
    VERBATIM)

add_custom_target(efiemu_image ALL DEPENDS ${EFIEMU_IMAGE})
install(FILES ${EFIEMU_ELF} ${EFIEMU_IMAGE} DESTINATION boot)
