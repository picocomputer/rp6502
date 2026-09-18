if(COMMAND rp6502_add_machine_test)
    rp6502_add_machine_test(modes FIRMWARE SPLIT
        SOURCES test_modes.c ${RP6502_BENCH}/mut_rtl.cpp
        DEFS ROMS_DIR="${RP6502_TEST_CORPUS}"
        TIMEOUT 900)
elseif(TARGET rp6502_libretro)
    rp6502_add_test(modes SPLIT LIBS ${CMAKE_DL_LIBS}
        SOURCES test_modes.c ${RP6502_BENCH}/mut_libretro.c
        INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
        DEFS ROMS_DIR="${RP6502_TEST_CORPUS}"
            RETRO_SO="$<TARGET_FILE:rp6502_libretro>"
        DEPENDS rp6502_libretro
        TIMEOUT 120)
else()
    rp6502_add_test(modes SPLIT LIBS emu_core
        SOURCES test_modes.c ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        DEFS ROMS_DIR="${RP6502_TEST_CORPUS}"
        TIMEOUT 120)
endif()

if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(banner FIRMWARE
            SOURCES test_banner.cpp ${RP6502_BENCH}/mut_rtl.cpp
            TIMEOUT 900)
    endif()
elseif(TARGET rp6502_libretro)
    rp6502_add_test(banner LIBS ${CMAKE_DL_LIBS}
        SOURCES test_banner.cpp ${RP6502_BENCH}/mut_libretro.c
        INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
        DEFS RETRO_SO="$<TARGET_FILE:rp6502_libretro>"
        DEPENDS rp6502_libretro
        TIMEOUT 120)
else()
    rp6502_add_test(banner LIBS emu_core
        SOURCES test_banner.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        TIMEOUT 60)
endif()

if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(session FIRMWARE
            SOURCES test_session.cpp ${RP6502_BENCH}/mut_rtl.cpp
            TIMEOUT 900)
    endif()
elseif(TARGET rp6502_libretro)
    rp6502_add_test(session LIBS ${CMAKE_DL_LIBS}
        SOURCES test_session.cpp ${RP6502_BENCH}/mut_libretro.c
        INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
        DEFS RETRO_SO="$<TARGET_FILE:rp6502_libretro>"
        DEPENDS rp6502_libretro
        TIMEOUT 120)
else()
    rp6502_add_test(session LIBS emu_core
        SOURCES test_session.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        TIMEOUT 60)
endif()

if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(compare FIRMWARE
            SOURCES test_compare.cpp ${RP6502_BENCH}/mut_rtl.cpp
            TIMEOUT 900)
    endif()
else()
    rp6502_add_test(compare LIBS emu_core
        SOURCES test_compare.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        TIMEOUT 60)
endif()

add_test(NAME vidmodes_corpus
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_TESTS_DIR}/gen/vidmodes_gate.py
        --manifest ${RP6502_TEST_CORPUS}/manifest.txt
        --suite ${CMAKE_CURRENT_LIST_DIR}/test_modes.c
        --suite ${RP6502_TESTS_DIR}/rtl/vga/test_vidregs.cpp)
set_tests_properties(vidmodes_corpus PROPERTIES LABELS "gate;cpu.vga")

if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(vsync FIRMWARE
            SOURCES test_vsync.cpp ${RP6502_BENCH}/mut_rtl.cpp
            DEFS VSYNC_ROM="${VSYNC_ROM}"
            DEPENDS vsync_rom
            TIMEOUT 900)
    endif()
elseif(TARGET rp6502_libretro)
    rp6502_add_test(vsync LIBS ${CMAKE_DL_LIBS}
        SOURCES test_vsync.cpp ${RP6502_BENCH}/mut_libretro.c
        INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
        DEFS VSYNC_ROM="${VSYNC_ROM}"
            RETRO_SO="$<TARGET_FILE:rp6502_libretro>"
        DEPENDS rp6502_libretro vsync_rom
        TIMEOUT 120)
else()
    rp6502_add_test(vsync LIBS emu_core
        SOURCES test_vsync.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        DEFS VSYNC_ROM="${VSYNC_ROM}"
        DEPENDS vsync_rom
        TIMEOUT 60)
endif()

if(NOT COMMAND rp6502_add_machine_test AND NOT TARGET rp6502_libretro)
    rp6502_add_test(progmap LIBS emu_core SOURCES test_progmap.c TIMEOUT 60)
endif()
