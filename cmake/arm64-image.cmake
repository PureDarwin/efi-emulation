# turns the arm64 ELF into a flat Image, run with cmake -P
# in: ELF OUT OBJCOPY READELF

execute_process(COMMAND ${READELF} -W -r ${ELF} OUTPUT_VARIABLE relocs RESULT_VARIABLE rc)
if(rc)
    message(FATAL_ERROR "readelf failed")
endif()

string(REGEX MATCHALL "R_AARCH64_[A-Z0-9_]+" types "${relocs}")
list(REMOVE_DUPLICATES types)
list(REMOVE_ITEM types R_AARCH64_RELATIVE R_AARCH64_NONE)
if(types)
    message(FATAL_ERROR "non-relative dynamic relocations: ${types}")
endif()

execute_process(COMMAND ${OBJCOPY} -O binary -R .bss ${ELF} ${OUT} RESULT_VARIABLE rc)
if(rc)
    message(FATAL_ERROR "objcopy failed")
endif()
