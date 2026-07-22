# CTest suite. Uses utest.h (vendored with sokol)

set(UTEST_DIR ${RP6502_VENDOR}/sokol/tests/functional)
set(ROMS ${CMAKE_CURRENT_SOURCE_DIR}/roms)
set(ADVENTURE ${ROMS}/adventure.rp6502)

# --- Unit tests (crc32, .rp6502 loader, xreg dispatch, CLI parser) ---
add_executable(test_units test_units.c ${RP6502_SRC}/emu/app/cli.c)
target_link_libraries(test_units PRIVATE emu_core)
target_include_directories(test_units PRIVATE ${UTEST_DIR})
target_compile_definitions(test_units PRIVATE ADVENTURE_ROM="${ADVENTURE}")
if(WIN32) # cli.c needs the same getopt the app uses
    target_link_libraries(test_units PRIVATE wingetopt)
    target_compile_definitions(test_units PRIVATE _BSD_SOURCE)
endif()
add_test(NAME units COMMAND test_units)

# --- Feature interfaces ($FFF0 SIGINT IRQ, launcher chain, teletype bell) ---
add_executable(test_features test_features.c)
target_link_libraries(test_features PRIVATE emu_core)
target_include_directories(test_features PRIVATE ${UTEST_DIR})
target_compile_definitions(test_features PRIVATE ADVENTURE_ROM="${ADVENTURE}")
add_test(NAME features COMMAND test_features)

# --- Adventure integration (ROM-asset files + interactive stdin via rln) ---
add_executable(test_adventure test_adventure.c)
target_link_libraries(test_adventure PRIVATE emu_core)
target_include_directories(test_adventure PRIVATE ${UTEST_DIR})
target_compile_definitions(test_adventure PRIVATE ADVENTURE_ROM="${ROMS}/adventure.rp6502")
add_test(NAME adventure COMMAND test_adventure)
set_tests_properties(adventure PROPERTIES TIMEOUT 60)

# --- Filesystem (host-backed MSC0: + read-only ROM: + directory enumeration) ---
add_executable(test_fs test_fs.c)
target_link_libraries(test_fs PRIVATE emu_core)
target_include_directories(test_fs PRIVATE ${UTEST_DIR})
add_test(NAME fs COMMAND test_fs)

# --- Drive fs.c paths: host-backed MSC0:, ROM overlay, async POSIX-AIO transfers ---
add_executable(test_drive test_drive.c)
target_link_libraries(test_drive PRIVATE emu_core)
target_include_directories(test_drive PRIVATE ${UTEST_DIR})
target_compile_definitions(test_drive PRIVATE ADVENTURE_ROM="${ADVENTURE}")
add_test(NAME drive COMMAND test_drive)

# --- Real FatFs on the emulator RAM disk (shared ria/api/fat.c + host/fat.c diskio) ---
add_executable(test_fatfs test_fatfs.c)
target_link_libraries(test_fatfs PRIVATE emu_core)
target_include_directories(test_fatfs PRIVATE ${UTEST_DIR})
add_test(NAME fatfs COMMAND test_fatfs)

# --- Directory enumeration integration (dir.rp6502: opendir/readdir/stat/getfree) ---
add_executable(test_dir test_dir.c)
target_link_libraries(test_dir PRIVATE emu_core)
target_include_directories(test_dir PRIVATE ${UTEST_DIR})
target_compile_definitions(test_dir PRIVATE DIR_ROM="${ROMS}/dir.rp6502")
add_test(NAME dir COMMAND test_dir)
set_tests_properties(dir PROPERTIES TIMEOUT 60)

# --- Master clock + PHI2 (reproducible run timing) ---
add_executable(test_clock test_clock.c)
target_link_libraries(test_clock PRIVATE emu_core)
target_include_directories(test_clock PRIVATE ${UTEST_DIR})
target_compile_definitions(test_clock PRIVATE ADVENTURE_ROM="${ADVENTURE}")
add_test(NAME clock COMMAND test_clock)

# --- Exec/argv (pro.c ops 0x08/0x09) + MSC0: program loading ---
add_executable(test_exec test_exec.c)
target_link_libraries(test_exec PRIVATE emu_core)
target_include_directories(test_exec PRIVATE ${UTEST_DIR})
target_compile_definitions(test_exec PRIVATE EXEC_ROM="${ROMS}/exec.rp6502")
add_test(NAME exec COMMAND test_exec)
set_tests_properties(exec PROPERTIES TIMEOUT 60)

# --- Debugger engine (dbg.c: run/stop/step + address breakpoints) ---
add_executable(test_dbg test_dbg.c)
target_link_libraries(test_dbg PRIVATE emu_core)
target_include_directories(test_dbg PRIVATE ${UTEST_DIR})
target_compile_definitions(test_dbg PRIVATE ADVENTURE_ROM="${ADVENTURE}")
add_test(NAME dbg COMMAND test_dbg)
set_tests_properties(dbg PROPERTIES TIMEOUT 60)

# --- DWARF5 .debug_info + .debug_line reader coverage (dwarf_info.c /
# dwarf_line.c) against dwtest.elf, an llvm-mos debug-fork -O0 -g build:
# base/array/struct/enum/pointer types, strx strings, DW_OP_addrx globals via
# .debug_addr, v5 unit/line headers, frame base + fbreg locals, and the MOS
# DWARF register numbering (frame_base = DW_OP_regx RS0 = 0x30000). Standalone:
# the parser only needs libc and the fixture is committed (no toolchain). ---
add_executable(test_dwarf5 test_dwarf5.c ${RP6502_SRC}/emu/dbg/dwarf_info.c ${RP6502_SRC}/emu/dbg/dwarf_line.c ${RP6502_SRC}/emu/dbg/dwarf_cursor.c ${RP6502_SRC}/emu/dbg/dwarf_elf.c)
target_include_directories(test_dwarf5 PRIVATE ${UTEST_DIR} ${RP6502_SRC})
target_compile_definitions(test_dwarf5 PRIVATE DW5_ELF="${ROMS}/dwtest.elf")
add_test(NAME dwarf5 COMMAND test_dwarf5)

# --- DWARF .debug_frame CFI unwinder (dwarf_frame.c): two-stack unwind. ---
add_executable(test_dwarf_frame test_dwarf_frame.c ${RP6502_SRC}/emu/dbg/dwarf_frame.c ${RP6502_SRC}/emu/dbg/dwarf_cursor.c ${RP6502_SRC}/emu/dbg/dwarf_elf.c)
target_include_directories(test_dwarf_frame PRIVATE ${UTEST_DIR} ${RP6502_SRC})
target_compile_definitions(test_dwarf_frame PRIVATE DW5_ELF="${ROMS}/dwtest.elf")
add_test(NAME dwarf_frame COMMAND test_dwarf_frame)

# --- cc65 .dbg reader (cc65dbg.c): line mapping + funcs + untyped variables.
# Fixture is the committed, hand-authored cc65.dbg (plain text, no toolchain). ---
add_executable(test_cc65dbg test_cc65dbg.c ${RP6502_SRC}/emu/dbg/cc65dbg.c)
target_include_directories(test_cc65dbg PRIVATE ${UTEST_DIR} ${RP6502_SRC})
target_compile_definitions(test_cc65dbg PRIVATE CC65_DBG="${ROMS}/cc65.dbg")
add_test(NAME cc65dbg COMMAND test_cc65dbg)

# --- W65C02S disassembler (vendor/chips/util/w65c02dasm.h, driving the ui_dbg
# disasm view). Header-only (CHIPS_UTIL_IMPL); no fixture/toolchain needed. ---
add_executable(test_w65c02dasm test_w65c02dasm.c)
target_include_directories(test_w65c02dasm PRIVATE ${UTEST_DIR} ${RP6502_SRC} ${RP6502_VENDOR})
add_test(NAME w65c02dasm COMMAND test_w65c02dasm)

# --- Clock/time API (host wall clock + timezone + locale + OEM code page) ---
add_executable(test_rtc test_rtc.c)
target_link_libraries(test_rtc PRIVATE emu_core)
target_include_directories(test_rtc PRIVATE ${UTEST_DIR})
target_compile_definitions(test_rtc PRIVATE RTC_ROM="${ROMS}/rtc.rp6502")
add_test(NAME rtc COMMAND test_rtc)
set_tests_properties(rtc PROPERTIES TIMEOUT 60)

# --- VGA graphics (one representative mode; the renderers share the scanline
# pipeline). mode2: tiled renderer + xreg + keyboard bitmap. ---
add_executable(test_mode2 test_mode2.c)
target_link_libraries(test_mode2 PRIVATE emu_core)
target_include_directories(test_mode2 PRIVATE ${UTEST_DIR})
target_compile_definitions(test_mode2 PRIVATE MODE2_ROM="${ROMS}/mode2.rp6502")
add_test(NAME mode2 COMMAND test_mode2)
set_tests_properties(mode2 PROPERTIES TIMEOUT 60)

# --- Audio (PSG via the ezpsg Fur Elise tracker) ---
add_executable(test_furelise test_furelise.c)
target_link_libraries(test_furelise PRIVATE emu_core)
target_include_directories(test_furelise PRIVATE ${UTEST_DIR})
target_compile_definitions(test_furelise PRIVATE
    FURELISE_ROM="${ROMS}/furelise.rp6502")
add_test(NAME furelise COMMAND test_furelise)
set_tests_properties(furelise PROPERTIES TIMEOUT 60)

# --- 6522 VIA timer IRQ + HID mouse, via the paint_mou demo ---
add_executable(test_paint_mou test_paint_mou.c)
target_link_libraries(test_paint_mou PRIVATE emu_core)
target_include_directories(test_paint_mou PRIVATE ${UTEST_DIR})
target_compile_definitions(test_paint_mou PRIVATE PAINT_MOU_ROM="${ROMS}/paint_mou.rp6502")
add_test(NAME paint_mou COMMAND test_paint_mou)
set_tests_properties(paint_mou PROPERTIES TIMEOUT 60)

# --- absolute pointer (tablet) window encoding, via paint_tab ---
add_executable(test_paint_tab test_paint_tab.c)
target_link_libraries(test_paint_tab PRIVATE emu_core)
target_include_directories(test_paint_tab PRIVATE ${UTEST_DIR})
target_compile_definitions(test_paint_tab PRIVATE PAINT_TAB_ROM="${ROMS}/paint_tab.rp6502")
add_test(NAME paint_tab COMMAND test_paint_tab)
set_tests_properties(paint_tab PROPERTIES TIMEOUT 60)

# --- Gamepad (HID pad xreg + pad_xram_t mirror), via the gamepad tester ---
add_executable(test_gamepad test_gamepad.c)
target_link_libraries(test_gamepad PRIVATE emu_core)
target_include_directories(test_gamepad PRIVATE ${UTEST_DIR})
target_compile_definitions(test_gamepad PRIVATE
    GAMEPAD_ROM="${ROMS}/gamepad.rp6502")
add_test(NAME gamepad COMMAND test_gamepad)
set_tests_properties(gamepad PROPERTIES TIMEOUT 60)
