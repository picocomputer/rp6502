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

# --- DWARF .debug_info variable/type model (dwarf_info.c). Standalone: the
# parser only needs libc, and the fixture (vars.elf, an llvm-mos -O0 -gdwarf-4
# build of vars.c) is committed, so this runs with no toolchain or window.
# vars.c is deliberately header-less so its line numbers match the committed
# .debug_line; prepending a license header shifts them and breaks dwarf_line. ---
add_executable(test_dwarf_info test_dwarf_info.c ${RP6502_SRC}/emu/dbg/dwarf_info.c ${RP6502_SRC}/emu/dbg/dwarf_cursor.c)
target_include_directories(test_dwarf_info PRIVATE ${UTEST_DIR} ${RP6502_SRC} ${RP6502_SRC}/emu ${RP6502_SRC}/emu/dbg)
target_compile_definitions(test_dwarf_info PRIVATE VARS_ELF="${ROMS}/vars.elf")
add_test(NAME dwarf_info COMMAND test_dwarf_info)

# --- DWARF .debug_line address<->source mapping (dwarf_line.c). Reuses the same
# committed vars.elf fixture (it also carries .debug_line + .symtab). ---
add_executable(test_dwarf_line test_dwarf_line.c ${RP6502_SRC}/emu/dbg/dwarf_line.c ${RP6502_SRC}/emu/dbg/dwarf_cursor.c)
target_include_directories(test_dwarf_line PRIVATE ${UTEST_DIR} ${RP6502_SRC} ${RP6502_SRC}/emu ${RP6502_SRC}/emu/dbg)
target_compile_definitions(test_dwarf_line PRIVATE VARS_ELF="${ROMS}/vars.elf")
add_test(NAME dwarf_line COMMAND test_dwarf_line)

# --- cc65 .dbg reader (cc65dbg.c): line mapping + funcs + untyped variables.
# Fixture is the committed, hand-authored cc65.dbg (plain text, no toolchain). ---
add_executable(test_cc65dbg test_cc65dbg.c ${RP6502_SRC}/emu/dbg/cc65dbg.c)
target_include_directories(test_cc65dbg PRIVATE ${UTEST_DIR} ${RP6502_SRC} ${RP6502_SRC}/emu ${RP6502_SRC}/emu/dbg)
target_compile_definitions(test_cc65dbg PRIVATE CC65_DBG="${ROMS}/cc65.dbg")
add_test(NAME cc65dbg COMMAND test_cc65dbg)

# --- W65C02S disassembler (the 65C02 fork driving the ui_dbg disasm view). ---
# Header-only (w65c02dasm.h with CHIPS_UTIL_IMPL); no fixture/toolchain needed.
add_executable(test_w65c02dasm test_w65c02dasm.c)
target_include_directories(test_w65c02dasm PRIVATE ${UTEST_DIR} ${RP6502_SRC} ${RP6502_SRC}/emu)
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

# --- 6522 VIA timer IRQ + HID mouse, via the paint demo ---
add_executable(test_paint test_paint.c)
target_link_libraries(test_paint PRIVATE emu_core)
target_include_directories(test_paint PRIVATE ${UTEST_DIR})
target_compile_definitions(test_paint PRIVATE PAINT_ROM="${ROMS}/paint.rp6502")
add_test(NAME paint COMMAND test_paint)
set_tests_properties(paint PROPERTIES TIMEOUT 60)

# --- Gamepad (HID pad xreg + pad_xram_t mirror), via the gamepad tester ---
add_executable(test_gamepad test_gamepad.c)
target_link_libraries(test_gamepad PRIVATE emu_core)
target_include_directories(test_gamepad PRIVATE ${UTEST_DIR})
target_compile_definitions(test_gamepad PRIVATE
    GAMEPAD_ROM="${ROMS}/gamepad.rp6502")
add_test(NAME gamepad COMMAND test_gamepad)
set_tests_properties(gamepad PROPERTIES TIMEOUT 60)
