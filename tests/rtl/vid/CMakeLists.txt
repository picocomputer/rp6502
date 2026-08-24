# The video path in the fabric: the raster, the pixel tail, and the five modes
# pixel-exact against the oracle.
#
# The mode ROMs are the shared corpus, generated once in rp6502_tests.cmake.
# tests/emu/vid boots each one and asserts a settled, non-empty picture; this
# tree runs the same files on both machines and demands pixel equality.

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
