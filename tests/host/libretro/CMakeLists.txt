# What this core owes a frontend, as opposed to what the machine owes a
# program. The machine's own claims are in tests/cpu and answer here too,
# through the same library these suites open.
if(NOT TARGET rp6502_libretro)
    return()
endif()

set(RP6502_RETRO_DEFS RETRO_SO="$<TARGET_FILE:rp6502_libretro>")

# The export surface, the system/av facts, and the things this core declines.
rp6502_add_test(abi
    SOURCES test_abi.c
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS}
    DEPENDS rp6502_libretro
    TIMEOUT 60)

# The verdict on an image, carried out as retro_load_game's own answer.
rp6502_add_test(load
    SOURCES test_load.cpp
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS}
    DEPENDS rp6502_libretro
    TIMEOUT 60)

# One frame per call, polled before read, a second of sound per second of
# frames — and the canvas the corpus names, not the one the machine claims.
rp6502_add_test(run
    SOURCES test_run.c
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS} ROMS_DIR="${RP6502_TEST_CORPUS}"
        FIXTURES_DIR="${RP6502_TEST_ROMS}"
        AUD_ROM_OPL="${AUD_ROM_OPL}"
    DEPENDS rp6502_libretro aud_roms
    TIMEOUT 60)

# The RetroPad, the keyboard, and the labels a frontend prints for them.
rp6502_add_test(input
    SOURCES test_input.c
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS} ROMS_DIR="${RP6502_TEST_ROMS}"
    DEPENDS rp6502_libretro
    TIMEOUT 60)

# The settings, and the frontends old enough to need them spelled differently.
rp6502_add_test(options
    SOURCES test_options.c
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS} ROMS_DIR="${RP6502_TEST_CORPUS}"
        FIXTURES_DIR="${RP6502_TEST_ROMS}"
    DEPENDS rp6502_libretro
    TIMEOUT 60)

# The two pointing devices, each reaching the block a program maps.
rp6502_add_test(pointer
    SOURCES test_pointer.c
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS} FIXTURES_DIR="${RP6502_TEST_ROMS}"
    DEPENDS rp6502_libretro
    TIMEOUT 60)
