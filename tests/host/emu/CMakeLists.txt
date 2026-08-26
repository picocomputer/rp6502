# The emulator as a program: its command line, its script channel, its
# debugger, and the parts of it that are not the machine.
#
# What belongs here is a claim about this host's program rather than about the
# RP6502 — the DAP and DWARF readers, the desktop gamepad policy above the
# per-OS seam, the rate pump between the machine's clock and an audio device,
# and the binary driven from outside. The scripts that pair with a subsystem
# live with that subsystem in tests/cpu; what is left is about the program.

# The exception: how many frames a script spends is the one thing a script
# cannot check about itself, so that one is a C test around script.c.
rp6502_add_test(script LIBS emu_core FIXTURE gamepad.rp6502
    SOURCES test_script.c ${RP6502_SRC}/host/sokol/script.c ${RP6502_SRC}/host/sokol/png.c ${RP6502_SRC}/host/sokol/gamepad.c
    TIMEOUT 60)

# Writing memory the program reads, rather than driving it through the inputs.
rp6502_add_script_test(poke FIXTURE gamepad.rp6502)

# The command line, which had no test at all: the script suites reach cli.c
# only through the options they happen to pass, so nothing was checking that a
# number it cannot read is refused rather than quietly becoming a default.
# Cheap enough that each one is its own name.
function(rp6502_add_cli_test name)
    add_test(NAME cli_${name} COMMAND rp6502-emu ${ARGN})
    set_tests_properties(cli_${name} PROPERTIES
        TIMEOUT 30 LABELS "cli;host.emu")
endfunction()

rp6502_add_cli_test(help --help)

foreach(_case
    "seed;--seed;x"
    "phi2;--phi2;fast"
    "scale;--scale;abc"
    "cp;--cp;4o7"
    "frames_without_screenshot;--frames;10")
    list(POP_FRONT _case _name)
    rp6502_add_cli_test(${_name} ${_case} ${RP6502_TEST_ROMS}/gamepad.rp6502)
    set_tests_properties(cli_${_name} PROPERTIES WILL_FAIL TRUE)
endforeach()

# A run that repeats: the seed reaches the fill, and the fill is read in the
# grammar the scripts and the docs use rather than C's, where 010 is eight.
add_test(NAME cli_fill_is_decimal COMMAND rp6502-emu
    --mute --seed 1 --fill 010
    --script ${CMAKE_CURRENT_LIST_DIR}/fill.txt
    ${RP6502_TEST_ROMS}/gamepad.rp6502)
set_tests_properties(cli_fill_is_decimal PROPERTIES LABELS "cli;host.emu")

# --- Unit tests (crc32, .rp6502 loader, xreg dispatch, CLI parser) ---
# Here rather than under cpu/ria because cli.c is this program's, and the
# loader and dispatch cases ride along in the same binary.
rp6502_add_test(units
    SOURCES test_units.c ${RP6502_SRC}/host/sokol/cli.c
    LIBS emu_core FIXTURE adventure.rp6502)
if(WIN32) # cli.c needs the same getopt the app uses
    target_link_libraries(test_units PRIVATE wingetopt)
    target_compile_definitions(test_units PRIVATE _BSD_SOURCE)
endif()

# --- aud_pump: the seam between the machine's rate and the host's ---
rp6502_add_test(pump LIBS emu_core TIMEOUT 60)

# --- The desktop gamepad policy, against a host that does as it is told ---
# Reading a controller is per-OS code no runner has hardware for. Everything
# above that seam is not, and the privacy gate and player numbering are the
# parts worth being sure of.
rp6502_add_test(gamepad_input LIBS emu_core
    SOURCES test_gamepad_input.c ${RP6502_SRC}/host/sokol/gamepad_input.c)

# --- The debugger: the engine, the DWARF and cc65 readers, the disassembler ---
# Their fixtures live here rather than in roms/ because none of them is a
# .rp6502 — dwtest.elf is an llvm-mos build and cc65.dbg is plain text.

# Debugger engine (dbg.c: run/stop/step + address breakpoints)
rp6502_add_test(dbg LIBS emu_core FIXTURE adventure.rp6502 TIMEOUT 60)

# DWARF5 .debug_info + .debug_line reader coverage (dwarf_info.c /
# dwarf_line.c) against dwtest.elf, an llvm-mos debug-fork -O0 -g build:
# base/array/struct/enum/pointer types, strx strings, DW_OP_addrx globals via
# .debug_addr, v5 unit/line headers, frame base + fbreg locals, and the MOS
# DWARF register numbering (frame_base = DW_OP_regx RS0 = 0x30000). Standalone:
# the parser only needs libc and the fixture is committed (no toolchain).
rp6502_add_test(dwarf5
    SOURCES test_dwarf5.c
        ${RP6502_SRC}/core/dap/dwarf_info.c ${RP6502_SRC}/core/dap/dwarf_line.c
        ${RP6502_SRC}/core/dap/dwarf_cursor.c ${RP6502_SRC}/core/dap/dwarf_elf.c
    INCLUDES ${RP6502_SRC} FIXTURE host/emu/dwtest.elf)

# DWARF .debug_frame CFI unwinder (dwarf_frame.c): two-stack unwind.
rp6502_add_test(dwarf_frame
    SOURCES test_dwarf_frame.c
        ${RP6502_SRC}/core/dap/dwarf_frame.c ${RP6502_SRC}/core/dap/dwarf_cursor.c
        ${RP6502_SRC}/core/dap/dwarf_elf.c
    INCLUDES ${RP6502_SRC} FIXTURE host/emu/dwtest.elf)

# cc65 .dbg reader (cc65dbg.c): line mapping + funcs + untyped variables.
# Fixture is the committed, hand-authored cc65.dbg (plain text, no toolchain).
rp6502_add_test(cc65dbg
    SOURCES test_cc65dbg.c ${RP6502_SRC}/core/dap/cc65dbg.c
    INCLUDES ${RP6502_SRC} FIXTURE host/emu/cc65.dbg)

# W65C02S disassembler (vendor/chips/util/w65c02dasm.h, driving the ui_dbg
# disasm view). Header-only (CHIPS_UTIL_IMPL); no fixture/toolchain needed.
rp6502_add_test(w65c02dasm INCLUDES ${RP6502_SRC} ${RP6502_VENDOR})
