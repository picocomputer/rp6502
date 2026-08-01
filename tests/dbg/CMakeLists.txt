# The debugger: the engine, the DWARF and cc65 readers, the disassembler.
#
# Their fixtures live here rather than in roms/ because none of them is a
# .rp6502 — dwtest.elf is an llvm-mos build and cc65.dbg is plain text.

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
    INCLUDES ${RP6502_SRC} FIXTURE dbg/dwtest.elf)

# --- DWARF .debug_frame CFI unwinder (dwarf_frame.c): two-stack unwind. ---
rp6502_add_test(dwarf_frame
    SOURCES test_dwarf_frame.c
        ${RP6502_SRC}/emu/dbg/dwarf_frame.c ${RP6502_SRC}/emu/dbg/dwarf_cursor.c
        ${RP6502_SRC}/emu/dbg/dwarf_elf.c
    INCLUDES ${RP6502_SRC} FIXTURE dbg/dwtest.elf)

# --- cc65 .dbg reader (cc65dbg.c): line mapping + funcs + untyped variables.
# Fixture is the committed, hand-authored cc65.dbg (plain text, no toolchain). ---
rp6502_add_test(cc65dbg
    SOURCES test_cc65dbg.c ${RP6502_SRC}/emu/dbg/cc65dbg.c
    INCLUDES ${RP6502_SRC} FIXTURE dbg/cc65.dbg)

# --- W65C02S disassembler (vendor/chips/util/w65c02dasm.h, driving the ui_dbg
# disasm view). Header-only (CHIPS_UTIL_IMPL); no fixture/toolchain needed. ---
rp6502_add_test(w65c02dasm INCLUDES ${RP6502_SRC} ${RP6502_VENDOR})
