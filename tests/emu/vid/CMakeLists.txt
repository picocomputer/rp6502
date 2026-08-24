# The video path in the emulator: the tiled renderer and the mode corpus.
#
# The mode ROMs are the shared corpus. This tree boots each one and asserts a
# settled, non-empty picture; tests/rtl/vid runs the same files on both
# machines and demands pixel equality. One generator, one corpus, two claims
# of different strength — which is why the ROMs are generated once in
# rp6502_tests.cmake rather than owned by either side.

# --- One representative mode through the whole emulator: the tiled renderer,
# xreg, and the keyboard bitmap, driven by a real program. ---
rp6502_add_test(tiles
    SOURCES test_tiles.c
    LIBS emu_core FIXTURE mode2.rp6502 TIMEOUT 60)

# --- The corpus in the emulator: every depth and canvas boots and settles ---
rp6502_add_test(vidmodes LIBS emu_core
    DEFS ROMS_DIR="${RP6502_TEST_CORPUS}" TIMEOUT 120)

# The corpus against both its readers. Neither suite can enumerate what
# vidmodes.py writes, and the case names are not the file names — test_modes
# names its cases for what they prove — so the lists stay written by hand and
# this says whether they are all there. Registered once, against both suites:
# the question has one answer, and this is the tree that runs on every push —
# tests/rtl is not configured on a machine with no Verilator.
add_test(NAME vidmodes_corpus
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_TESTS_DIR}/gen/vidmodes_gate.py
        --manifest ${RP6502_TEST_CORPUS}/manifest.txt
        --suite ${CMAKE_CURRENT_LIST_DIR}/test_vidmodes.c
        --suite ${RP6502_TESTS_DIR}/rtl/vid/test_modes.cpp)
set_tests_properties(vidmodes_corpus PROPERTIES LABELS "gate;vid")
