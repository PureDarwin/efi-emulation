# packs ELF into a bzImage, run with cmake -P
# in: ELF OUT OBJCOPY NM NASM SETUP

get_filename_component(dir ${OUT} DIRECTORY)
set(payload ${dir}/efi-emulation.payload)
set(setup ${dir}/efi-emulation.setup)

execute_process(COMMAND ${OBJCOPY} -O binary -R .bss ${ELF} ${payload} RESULT_VARIABLE rc)
if(rc)
    message(FATAL_ERROR "objcopy failed")
endif()

execute_process(COMMAND ${NM} ${ELF} OUTPUT_VARIABLE symbols RESULT_VARIABLE rc)
if(rc)
    message(FATAL_ERROR "nm failed")
endif()

function(symbol name var)
    string(REGEX MATCH "([0-9a-fA-F]+) [A-Za-z] ${name}\n" hit "${symbols}")
    if(NOT hit)
        message(FATAL_ERROR "no ${name} in ${ELF}")
    endif()
    math(EXPR value "0x${CMAKE_MATCH_1}" OUTPUT_FORMAT DECIMAL)
    set(${var} ${value} PARENT_SCOPE)
endfunction()

symbol(__kernel_start start)
symbol(__file_end file_end)
symbol(__kernel_end kernel_end)

file(SIZE ${payload} payload_size)
math(EXPR expected "${file_end} - ${start}")
if(NOT payload_size EQUAL expected)
    message(FATAL_ERROR "payload size mismatch, ${payload_size} bytes but __file_end says ${expected}")
endif()

# the setup header counts the payload in 16 byte paragraphs, init size in pages
math(EXPR syssize "(${payload_size} + 15) / 16")
math(EXPR init_size "((${kernel_end} - ${start}) + 4095) & ~4095")

execute_process(COMMAND ${NASM} -f bin -DSYSSIZE=${syssize} -DINIT_SIZE=${init_size}
    ${SETUP} -o ${setup} RESULT_VARIABLE rc)
if(rc)
    message(FATAL_ERROR "nasm failed on ${SETUP}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E cat ${setup} ${payload} OUTPUT_FILE ${OUT} RESULT_VARIABLE rc)
if(rc)
    message(FATAL_ERROR "cannot write ${OUT}")
endif()
