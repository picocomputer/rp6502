#
# Copyright (c) 2026 Rumbledethumps
# SPDX-License-Identifier: BSD-3-Clause
#
# CTest suite. Uses utest.h (vendored with sokol) — no new dependency.
#

set(UTEST_DIR ${SOKOL}/tests/functional)
set(ROMS ${CMAKE_CURRENT_SOURCE_DIR}/roms)
set(HELLO ${ROMS}/hello.rp6502)

# Framebuffer CRC of hello.rp6502 after 2 frames. If the rendering legitimately
# changes, run test_golden to print the new value and update this line.
set(GOLDEN_HELLO_CRC "0x7CDBBBB9")

# --- CPU conformance (Klaus Dormann). Standalone: own CPU impl + flat RAM. ---
add_executable(test_cpu_conformance test_cpu_conformance.c)
target_include_directories(test_cpu_conformance PRIVATE ${SRC} ${EMU} ${EMU}/dbg)
add_test(NAME cpu_6502_functional
    COMMAND test_cpu_conformance ${ROMS}/6502_functional_test.bin 3469)
add_test(NAME cpu_65c02_extended
    COMMAND test_cpu_conformance ${ROMS}/65C02_extended_opcodes_test.bin 24F1)
set_tests_properties(cpu_6502_functional cpu_65c02_extended PROPERTIES TIMEOUT 120)

# --- Golden integration (whole headless pipeline) ---
add_executable(test_golden test_golden.c)
target_link_libraries(test_golden PRIVATE emu_core)
target_include_directories(test_golden PRIVATE ${UTEST_DIR})
target_compile_definitions(test_golden PRIVATE
    HELLO_ROM="${HELLO}"
    GOLDEN_HELLO_CRC=${GOLDEN_HELLO_CRC})
add_test(NAME golden_hello COMMAND test_golden)

# --- Unit tests (crc32, .rp6502 loader, xreg dispatch) ---
add_executable(test_units test_units.c)
target_link_libraries(test_units PRIVATE emu_core)
target_include_directories(test_units PRIVATE ${UTEST_DIR})
target_compile_definitions(test_units PRIVATE HELLO_ROM="${HELLO}")
add_test(NAME units COMMAND test_units)

# --- Feature interfaces ($FFF0 SIGINT IRQ, launcher chain, teletype bell) ---
add_executable(test_features test_features.c)
target_link_libraries(test_features PRIVATE emu_core)
target_include_directories(test_features PRIVATE ${UTEST_DIR})
target_compile_definitions(test_features PRIVATE HELLO_ROM="${HELLO}")
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
target_compile_definitions(test_drive PRIVATE HELLO_ROM="${HELLO}")
add_test(NAME drive COMMAND test_drive)

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
target_compile_definitions(test_clock PRIVATE HELLO_ROM="${HELLO}")
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
target_compile_definitions(test_dbg PRIVATE HELLO_ROM="${HELLO}")
add_test(NAME dbg COMMAND test_dbg)

# --- DWARF .debug_info variable/type model (dwarf_info.c). Standalone: the
# parser only needs libc, and the fixture (vars.elf, an llvm-mos -O0 -gdwarf-4
# build of vars.c) is committed, so this runs with no toolchain or window. ---
add_executable(test_dwarf_info test_dwarf_info.c ${EMU}/dbg/dwarf_info.c)
target_include_directories(test_dwarf_info PRIVATE ${UTEST_DIR} ${SRC} ${EMU} ${EMU}/dbg)
target_compile_definitions(test_dwarf_info PRIVATE VARS_ELF="${ROMS}/vars.elf")
add_test(NAME dwarf_info COMMAND test_dwarf_info)

# --- DWARF .debug_line address<->source mapping (dwarf_line.c). Reuses the same
# committed vars.elf fixture (it also carries .debug_line + .symtab). ---
add_executable(test_dwarf_line test_dwarf_line.c ${EMU}/dbg/dwarf_line.c)
target_include_directories(test_dwarf_line PRIVATE ${UTEST_DIR} ${SRC} ${EMU} ${EMU}/dbg)
target_compile_definitions(test_dwarf_line PRIVATE VARS_ELF="${ROMS}/vars.elf")
add_test(NAME dwarf_line COMMAND test_dwarf_line)

# --- cc65 .dbg reader (cc65dbg.c): line mapping + funcs + untyped variables.
# Fixture is the committed, hand-authored cc65.dbg (plain text, no toolchain). ---
add_executable(test_cc65dbg test_cc65dbg.c ${EMU}/dbg/cc65dbg.c)
target_include_directories(test_cc65dbg PRIVATE ${UTEST_DIR} ${SRC} ${EMU} ${EMU}/dbg)
target_compile_definitions(test_cc65dbg PRIVATE CC65_DBG="${ROMS}/cc65.dbg")
add_test(NAME cc65dbg COMMAND test_cc65dbg)

# --- W65C02S disassembler (the 65C02 fork driving the ui_dbg disasm view). ---
# Header-only (w65c02dasm.h with CHIPS_UTIL_IMPL); no fixture/toolchain needed.
add_executable(test_w65c02dasm test_w65c02dasm.c)
target_include_directories(test_w65c02dasm PRIVATE ${UTEST_DIR} ${SRC} ${EMU} ${EMU}/dbg)
add_test(NAME w65c02dasm COMMAND test_w65c02dasm)

# --- Clock/time API (host wall clock + timezone + locale + OEM code page) ---
add_executable(test_rtc test_rtc.c)
target_link_libraries(test_rtc PRIVATE emu_core)
target_include_directories(test_rtc PRIVATE ${UTEST_DIR})
target_compile_definitions(test_rtc PRIVATE RTC_ROM="${ROMS}/rtc.rp6502")
add_test(NAME rtc COMMAND test_rtc)
set_tests_properties(rtc PROPERTIES TIMEOUT 60)

# --- VGA graphics mode 1 (tile/text renderer + xreg + keyboard bitmap) ---
add_executable(test_mode1 test_mode1.c)
target_link_libraries(test_mode1 PRIVATE emu_core)
target_include_directories(test_mode1 PRIVATE ${UTEST_DIR})
target_compile_definitions(test_mode1 PRIVATE MODE1_ROM="${ROMS}/mode1.rp6502")
add_test(NAME mode1 COMMAND test_mode1)
set_tests_properties(mode1 PROPERTIES TIMEOUT 60)

# --- VGA graphics mode 2 (tiled renderer + xreg + keyboard bitmap) ---
add_executable(test_mode2 test_mode2.c)
target_link_libraries(test_mode2 PRIVATE emu_core)
target_include_directories(test_mode2 PRIVATE ${UTEST_DIR})
target_compile_definitions(test_mode2 PRIVATE MODE2_ROM="${ROMS}/mode2.rp6502")
add_test(NAME mode2 COMMAND test_mode2)
set_tests_properties(mode2 PROPERTIES TIMEOUT 60)

# --- VGA graphics mode 3 (bitmap renderer, 320x180 canvas) ---
add_executable(test_mode3 test_mode3.c)
target_link_libraries(test_mode3 PRIVATE emu_core)
target_include_directories(test_mode3 PRIVATE ${UTEST_DIR})
target_compile_definitions(test_mode3 PRIVATE MODE3_ROM="${ROMS}/mode3.rp6502")
add_test(NAME mode3 COMMAND test_mode3)
set_tests_properties(mode3 PROPERTIES TIMEOUT 60)

# --- VGA graphics mode 4 (affine sprites; software interpolator off-device) ---
add_executable(test_mode4 test_mode4.c)
target_link_libraries(test_mode4 PRIVATE emu_core)
target_include_directories(test_mode4 PRIVATE ${UTEST_DIR})
target_compile_definitions(test_mode4 PRIVATE MODE4_ROM="${ROMS}/affine.rp6502")
add_test(NAME mode4 COMMAND test_mode4)
set_tests_properties(mode4 PROPERTIES TIMEOUT 60)

# --- VGA graphics mode 5 (sprites + multi-plane alpha compositing) ---
add_executable(test_mode5 test_mode5.c)
target_link_libraries(test_mode5 PRIVATE emu_core)
target_include_directories(test_mode5 PRIVATE ${UTEST_DIR})
target_compile_definitions(test_mode5 PRIVATE MODE5_ROM="${ROMS}/mode5.rp6502")
add_test(NAME mode5 COMMAND test_mode5)
set_tests_properties(mode5 PROPERTIES TIMEOUT 60)

# --- Audio (PSG via the ezpsg Fur Elise tracker) ---
add_executable(test_furelise test_furelise.c)
target_link_libraries(test_furelise PRIVATE emu_core)
target_include_directories(test_furelise PRIVATE ${UTEST_DIR})
target_compile_definitions(test_furelise PRIVATE
    FURELISE_ROM="${ROMS}/furelise.rp6502"
    HELLO_ROM="${HELLO}")
add_test(NAME furelise COMMAND test_furelise)
set_tests_properties(furelise PROPERTIES TIMEOUT 60)

# --- 6522 VIA timer IRQ + HID mouse, via the paint demo ---
add_executable(test_paint test_paint.c)
target_link_libraries(test_paint PRIVATE emu_core)
target_include_directories(test_paint PRIVATE ${UTEST_DIR})
target_compile_definitions(test_paint PRIVATE PAINT_ROM="${ROMS}/paint.rp6502")
add_test(NAME paint COMMAND test_paint)
set_tests_properties(paint PROPERTIES TIMEOUT 60)

# --- Gamepad (HID pad xreg + pad_xram_t mirror) + OPL2 music, via Lode Runner ---
add_executable(test_loderunner test_loderunner.c)
target_link_libraries(test_loderunner PRIVATE emu_core)
target_include_directories(test_loderunner PRIVATE ${UTEST_DIR})
target_compile_definitions(test_loderunner PRIVATE
    LODERUNNER_ROM="${ROMS}/loderunner.rp6502")
add_test(NAME loderunner COMMAND test_loderunner)
set_tests_properties(loderunner PROPERTIES TIMEOUT 60)
