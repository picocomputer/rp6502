# The video path: the raster's product, the five modes, the font store and
# the terminal.
#
# The mode ROMs are the corpus, generated once in rp6502_tests.cmake, and the
# manifest beside them states each one's canvas. test_modes.c boots every one
# of them and checks the frame against the CRC written down in the case —
# the same source and the same numbers whichever machine this tree builds,
# which is what mut.h exists for.

# --- One representative mode through the whole emulator: the tiled renderer,
# xreg, and the keyboard bitmap, driven by a real program. ---
rp6502_add_test(tiles
    SOURCES test_tiles.c
    LIBS emu_core FIXTURE mode2.rp6502 TIMEOUT 60)

# --- The whole corpus, frame by frame, against the CRC in each case ---
# One source, three bindings: the tree that has a simulator runs it on the
# verilated machine and splits the cases, because each is a simulation worth
# naming on its own; the tree that ships a libretro core asks the core, which
# is the shipped library and not a pile of its objects; the tree that has
# emu_core runs the same cases in a fifth of a second. None is another's
# oracle — all three answer to the numbers in test_modes.c.
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

# The terminal end to end: one banner program, the frame it leaves.
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

# A scripted terminal session, and the picture it settles to.
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

# The syscall ABI's console stream, on whichever machine this tree built.
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

# The corpus against its readers. The suites cannot enumerate what vidmodes.py
# writes, and the case names are not the file names — they name what they
# prove — so the lists stay written by hand and this says whether they are all
# there. Two suites, because one fixture's picture is a fabric claim and is
# asserted where that machine is: see tests/rtl/vga.
add_test(NAME vidmodes_corpus
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_TESTS_DIR}/gen/vidmodes_gate.py
        --manifest ${RP6502_TEST_CORPUS}/manifest.txt
        --suite ${CMAKE_CURRENT_LIST_DIR}/test_modes.c
        --suite ${RP6502_TESTS_DIR}/rtl/vga/test_vidregs.cpp)
set_tests_properties(vidmodes_corpus PROPERTIES LABELS "gate;cpu.vga")

if(COMMAND rp6502_add_machine_test)
    # The frame counter and the vsync interrupt against the raster.
    rp6502_add_machine_test(vsync TIMEOUT 120)
endif()
