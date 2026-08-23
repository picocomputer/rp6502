# The video path: the raster, the five modes, the font store and the terminal.
#
# The mode ROMs are the shared corpus. The emulator boots each one and asserts
# a settled, non-empty picture; the RTL suite runs the same files on both
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
# this says whether they are all there. Named in the emulator tree too, where
# test_modes.cpp is never compiled: the question has one answer either way and
# this is the tree that runs on every push.
add_test(NAME vidmodes_corpus
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_TESTS_DIR}/gen/vidmodes_gate.py
        --manifest ${RP6502_TEST_CORPUS}/manifest.txt
        --suite ${CMAKE_CURRENT_LIST_DIR}/test_vidmodes.c
        --suite ${CMAKE_CURRENT_LIST_DIR}/test_modes.cpp)
set_tests_properties(vidmodes_corpus PROPERTIES LABELS "gate;vid")

if(RP6502_VERILATE)
    # The 640x480@60 raster at the two-clock render tick. The only test that
    # reads waveforms, so the only one built with tracing.
    rp6502_add_machine_test(raster TRACE
        SOURCES test_raster.cpp ${RP6502_BENCH}/tb_core.cpp)

    # The frame counter and the vsync interrupt against the raster.
    rp6502_add_machine_test(vsync TIMEOUT 120)

    # The shared pixel tail against a golden model, before any mode front
    # stands on it: every depth, both bit orders, wrap-split, tile-shaped,
    # immediate and mixed segments, under a jittered grant line.
    rp6502_add_module_test(pixtail
        TOP vid_pixtail
        RTL ${VID_PALETTE_PKG} ${RP6502_SRC}/core/vga/vid_pixtail.sv
        INCLUDES ${RP6502_ASSETS} ${RP6502_TEST_TABLES}
        DEPENDS vid_palette_rom vid_palette_tables
        TIMEOUT 120)

    # The generated font ROM against the tables emu_core builds at runtime.
    rp6502_add_test(font
        SOURCES test_font.cpp
        INCLUDES ${RP6502_ASSETS} ${RP6502_TEST_TABLES} ${RP6502_SRC}
        LIBS fpga_oracle)
    add_dependencies(test_font vid_font_tables vid_palette_tables aud_sine_tables)
endif()

if(RP6502_SOFT_CPU)
    # The M3 frame test: one banner ROM, two machines, pixel-exact.
    rp6502_add_machine_test(banner ORACLE FIRMWARE TIMEOUT 900)

    # The M2 exit test: one .rp6502 image on both, console streams compared
    # byte for byte.
    rp6502_add_machine_test(compare ORACLE FIRMWARE TIMEOUT 900)

    # The M3 scripted session settled and pixel-exact, plus the RTL-relative
    # blink phase check.
    rp6502_add_machine_test(session ORACLE FIRMWARE TIMEOUT 900)

    # The glyphs end to end: the asset beside the core reaches the store.
    rp6502_add_machine_test(fontstore ORACLE FIRMWARE TIMEOUT 900)

    # The corpus again, this time pixel-exact against the oracle: mode 1
    # character cells per format; mode 2 tile maps per depth and tile size
    # with trims, wraps and the composite; mode 3 bitmaps per depth and
    # canvas; mode 4 raw sixteen-bit sprites with opacity metadata; mode 5
    # the foreground walk, clips and per-sprite palettes. One binary, and
    # SPLIT gives each fixture its own ctest name — mode1.* through
    # mode5.*, the names the five separate suites used to register. It
    # also measures what the render spends — the engines race the beam —
    # on the fixtures heavy enough to be worth asking, because those are
    # these fixtures and a second suite over them was a second boot.
    rp6502_add_machine_test(modes ORACLE FIRMWARE SPLIT
        DEFS ROMS_DIR="${RP6502_TEST_CORPUS}"
        TIMEOUT 900)
endif()
