include(${CMAKE_CURRENT_LIST_DIR}/../rp6502_tests.cmake)

# --- Unit tests (crc32, .rp6502 loader, xreg dispatch, CLI parser) ---
rp6502_add_test(units
    SOURCES test_units.c ${RP6502_SRC}/emu/app/cli.c
    LIBS emu_core FIXTURE adventure.rp6502)
if(WIN32) # cli.c needs the same getopt the app uses
    target_link_libraries(test_units PRIVATE wingetopt)
    target_compile_definitions(test_units PRIVATE _BSD_SOURCE)
endif()

# --- The resampler, on its own: no emulator, no 6502, just arithmetic and a
# ruler. Measures the filter rather than asserting it, so a change that
# quietly makes it worse fails here instead of on someone's headphones. ---
rp6502_add_test(rsmp
    SOURCES test_rsmp.c ${RP6502_SRC}/ria/aud/rsmp.c
    INCLUDES ${RP6502_SRC})
if(NOT WIN32)
    target_link_libraries(test_rsmp PRIVATE m)
endif()

# --- Feature interfaces ($FFF0 SIGINT IRQ, launcher chain, teletype bell) ---
rp6502_add_test(features LIBS emu_core FIXTURE adventure.rp6502)

# --- Adventure integration (ROM-asset files + interactive stdin via rln) ---
rp6502_add_test(adventure LIBS emu_core FIXTURE adventure.rp6502 TIMEOUT 60)

# --- Filesystem (host-backed MSC0: + read-only ROM: + directory enumeration) ---
rp6502_add_test(fs LIBS emu_core)

# --- Drive fs.c paths: host-backed MSC0:, ROM overlay, async POSIX-AIO transfers ---
rp6502_add_test(drive LIBS emu_core FIXTURE adventure.rp6502)

# --- Real FatFs on the emulator RAM disk (shared ria/api/fat.c + host/fat.c diskio) ---
rp6502_add_test(fatfs LIBS emu_core)

# --- Directory enumeration integration (dir.rp6502: opendir/readdir/stat/getfree) ---
rp6502_add_test(dir LIBS emu_core FIXTURE dir.rp6502 TIMEOUT 60)

# --- Master clock + PHI2 (reproducible run timing) ---
rp6502_add_test(clock LIBS emu_core FIXTURE adventure.rp6502)

# --- Exec/argv (pro.c ops 0x08/0x09) + MSC0: program loading ---
rp6502_add_test(exec LIBS emu_core FIXTURE exec.rp6502 TIMEOUT 60)

# --- Debugger engine (dbg.c: run/stop/step + address breakpoints) ---
rp6502_add_test(dbg LIBS emu_core FIXTURE adventure.rp6502 TIMEOUT 60)

# --- DWARF5 .debug_info + .debug_line reader coverage (dwarf_info.c /
# dwarf_line.c) against dwtest.elf, an llvm-mos debug-fork -O0 -g build:
# base/array/struct/enum/pointer types, strx strings, DW_OP_addrx globals via
# .debug_addr, v5 unit/line headers, frame base + fbreg locals, and the MOS
# DWARF register numbering (frame_base = DW_OP_regx RS0 = 0x30000). Standalone:
# the parser only needs libc and the fixture is committed (no toolchain). ---
rp6502_add_test(dwarf5
    SOURCES test_dwarf5.c
        ${RP6502_SRC}/emu/dbg/dwarf_info.c ${RP6502_SRC}/emu/dbg/dwarf_line.c
        ${RP6502_SRC}/emu/dbg/dwarf_cursor.c ${RP6502_SRC}/emu/dbg/dwarf_elf.c
    INCLUDES ${RP6502_SRC} FIXTURE dwtest.elf)

# --- DWARF .debug_frame CFI unwinder (dwarf_frame.c): two-stack unwind. ---
rp6502_add_test(dwarf_frame
    SOURCES test_dwarf_frame.c
        ${RP6502_SRC}/emu/dbg/dwarf_frame.c ${RP6502_SRC}/emu/dbg/dwarf_cursor.c
        ${RP6502_SRC}/emu/dbg/dwarf_elf.c
    INCLUDES ${RP6502_SRC} FIXTURE dwtest.elf)

# --- cc65 .dbg reader (cc65dbg.c): line mapping + funcs + untyped variables.
# Fixture is the committed, hand-authored cc65.dbg (plain text, no toolchain). ---
rp6502_add_test(cc65dbg
    SOURCES test_cc65dbg.c ${RP6502_SRC}/emu/dbg/cc65dbg.c
    INCLUDES ${RP6502_SRC} FIXTURE cc65.dbg)

# --- W65C02S disassembler (vendor/chips/util/w65c02dasm.h, driving the ui_dbg
# disasm view). Header-only (CHIPS_UTIL_IMPL); no fixture/toolchain needed. ---
rp6502_add_test(w65c02dasm INCLUDES ${RP6502_SRC} ${RP6502_VENDOR})

# --- Clock/time API (host wall clock + timezone + locale + OEM code page) ---
rp6502_add_test(rtc LIBS emu_core FIXTURE rtc.rp6502 TIMEOUT 60)

# --- VGA graphics (one representative mode; the renderers share the scanline
# pipeline). mode2: tiled renderer + xreg + keyboard bitmap. ---
rp6502_add_test(mode2 LIBS emu_core FIXTURE mode2.rp6502 TIMEOUT 60)

# --- The video-mode corpus (mode 1 and mode 3, every depth and canvas).
# The FPGA suite runs these same ROMs and demands pixel equality. ---
rp6502_add_test(vidmodes LIBS emu_core
    DEFS ROMS_DIR="${RP6502_TEST_ROMS}" TIMEOUT 120)

# --- Audio (PSG via the ezpsg Fur Elise tracker) ---
rp6502_add_test(furelise LIBS emu_core FIXTURE furelise.rp6502 TIMEOUT 60)

# --- 6522 VIA timer IRQ + HID mouse, via the paint_mou demo ---
rp6502_add_test(paint_mou LIBS emu_core FIXTURE paint_mou.rp6502 TIMEOUT 60)

# --- absolute pointer (tablet) window encoding, via paint_tab ---
rp6502_add_test(paint_tab LIBS emu_core FIXTURE paint_tab.rp6502 TIMEOUT 60)

# --- Gamepad (HID pad xreg + pad_xram_t mirror), via the gamepad tester ---
rp6502_add_test(gamepad LIBS emu_core FIXTURE gamepad.rp6502 TIMEOUT 60)

# --- OEM code page conversion (ria/api/uni.c) against the ffunicode.c it
# replaces. The vendored file is compiled beside it with its entry points
# renamed, so every code page, every byte, every code point in the BMP,
# and the whole up-case map are compared in one process. ---
set_source_files_properties(${OEMCP_C} PROPERTIES GENERATED TRUE)
rp6502_add_test(uni
    SOURCES test_uni.c ${RP6502_SRC}/ria/api/uni.c ${OEMCP_C}
        ${RP6502_VENDOR}/fatfs/ffunicode.c
    INCLUDES ${RP6502_SRC} ${RP6502_VENDOR} ${RP6502_SRC}/emu/host/pico
        ${OEMCP_DIR})
add_dependencies(test_uni oemcp)
set_source_files_properties(${RP6502_VENDOR}/fatfs/ffunicode.c
    TARGET_DIRECTORY test_uni PROPERTIES COMPILE_DEFINITIONS
    "ff_oem2uni=ref_oem2uni;ff_uni2oem=ref_uni2oem;ff_wtoupper=ref_wtoupper")
