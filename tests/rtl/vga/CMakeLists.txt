rp6502_add_machine_test(raster TRACE
    SOURCES test_raster.cpp ${RP6502_BENCH}/tb_core.cpp)

rp6502_add_machine_test(vsync_rtl SOURCES test_vsync_rtl.cpp TIMEOUT 120)

rp6502_add_module_test(pixtail
    TOP pixtail
    RTL ${VID_PALETTE_PKG} ${RP6502_SRC}/core/vga/scan/pixtail.sv
    INCLUDES ${RP6502_ASSETS} ${RP6502_TEST_TABLES}
    DEPENDS vid_palette_rom vid_palette_tables
    TIMEOUT 120)

rp6502_add_test(font
    SOURCES test_font.cpp
    INCLUDES ${RP6502_ASSETS} ${RP6502_TEST_TABLES} ${RP6502_SRC} ${RP6502_BENCH}
    LIBS emu_core)
add_dependencies(test_font vid_font_tables vid_palette_tables aud_sine_tables)

rp6502_add_machine_test(vidregs FIRMWARE
    SOURCES test_vidregs.cpp
    DEFS ROMS_DIR="${RP6502_TEST_CORPUS}"
    TIMEOUT 900)

rp6502_add_machine_test(fontstore FIRMWARE
    SOURCES test_fontstore.cpp
    TIMEOUT 900)
target_link_libraries(test_fontstore PRIVATE emu_core)

rp6502_add_machine_test(blink FIRMWARE
    SOURCES test_blink.cpp
    INCLUDES ${RP6502_TESTS_DIR}/cpu/vga
    TIMEOUT 900)
