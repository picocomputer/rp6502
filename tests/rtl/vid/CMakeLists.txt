# The video path by methods only a simulator has: the beam itself, a module
# below any mode, the store nothing reads back, and two registers inside the
# path that no frame can show.
#
# What the modes render is a machine claim and lives in tests/cpu/vid.

# The 640x480@60 raster at the two-clock render tick. The only test that
# reads waveforms, so the only one built with tracing.
rp6502_add_machine_test(raster TRACE
    SOURCES test_raster.cpp ${RP6502_BENCH}/tb_core.cpp)

# The shared pixel tail against a golden model, before any mode front
# stands on it: every depth, both bit orders, wrap-split, tile-shaped,
# immediate and mixed segments, under a jittered grant line.
rp6502_add_module_test(pixtail
    TOP vid_pixtail
    RTL ${VID_PALETTE_PKG} ${RP6502_SRC}/core/vga/vid_pixtail.sv
    INCLUDES ${RP6502_ASSETS} ${RP6502_TEST_TABLES}
    DEPENDS vid_palette_rom vid_palette_tables
    TIMEOUT 120)

# The generated tables against the ones emu_core builds at runtime. No machine
# runs — this compares two build artifacts — but the generated headers are this
# tree's own, so it is the only tree that can ask.
rp6502_add_test(font
    SOURCES test_font.cpp
    INCLUDES ${RP6502_ASSETS} ${RP6502_TEST_TABLES} ${RP6502_SRC} ${RP6502_BENCH}
    LIBS emu_core)
add_dependencies(test_font vid_font_tables vid_palette_tables aud_sine_tables)

# Two registers inside the video path. Both ride corpus ROMs whose pictures
# tests/cpu/vid already checks on both machines; what is left over is what
# only this one has.
rp6502_add_machine_test(vidregs FIRMWARE
    SOURCES test_vidregs.cpp
    DEFS ROMS_DIR="${RP6502_TEST_CORPUS}"
    TIMEOUT 900)

# The glyphs end to end: the asset beside the core reaches the fabric's font
# store. Every assertion reads that store, which only this machine has, so the
# whole test is a fabric claim; emu_core comes along for its tables.
rp6502_add_machine_test(fontstore FIRMWARE
    SOURCES test_fontstore.cpp
    TIMEOUT 900)
target_link_libraries(test_fontstore PRIVATE emu_core)

# The cell blink, held still by hand. It boots the scripted session from
# tests/cpu/vid for the state that script leaves behind; the picture itself is
# checked there, on both machines.
rp6502_add_machine_test(blink FIRMWARE
    SOURCES test_blink.cpp
    INCLUDES ${RP6502_TESTS_DIR}/cpu/vid
    TIMEOUT 900)
