if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(rw FIRMWARE
            SOURCES test_rw.cpp ${RP6502_BENCH}/mut_rtl.cpp
            TIMEOUT 900)
    endif()
else()
    rp6502_add_test(rw LIBS emu_core
        SOURCES test_rw.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        TIMEOUT 60)
endif()

if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(xram FIRMWARE
            SOURCES test_xram.cpp ${RP6502_BENCH}/mut_rtl.cpp
            TIMEOUT 900)
    endif()
elseif(TARGET rp6502_libretro)
    rp6502_add_test(xram LIBS ${CMAKE_DL_LIBS}
        SOURCES test_xram.cpp ${RP6502_BENCH}/mut_libretro.c
        INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
        DEFS RETRO_SO="$<TARGET_FILE:rp6502_libretro>"
        DEPENDS rp6502_libretro
        TIMEOUT 120)
else()
    rp6502_add_test(xram LIBS emu_core
        SOURCES test_xram.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        TIMEOUT 60)
endif()
