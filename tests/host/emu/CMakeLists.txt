rp6502_add_test(script LIBS emu_core ROM ${GAMEPAD_ROM} DEPENDS gamepad_roms
    SOURCES test_script.c ${RP6502_SRC}/host/sokol/cli/script.c
        ${RP6502_SRC}/host/sokol/cli/expr.c
        ${RP6502_SRC}/host/sokol/cli/png.c ${RP6502_SRC}/host/sokol/cli/state.c
    TIMEOUT 60)

rp6502_add_script_test(NAME poke COMMAND poke.txt ${GAMEPAD_ROM})

rp6502_add_script_test(NAME state COMMAND state.txt ${GAMEPAD_ROM})

rp6502_add_script_test(NAME state_mode COMMAND state_mode.txt
    ${RP6502_TEST_CORPUS}/mode3_8bpp.rp6502)

rp6502_add_script_test(NAME split COMMAND split.txt ${STDIO_ROM})

rp6502_add_script_test(NAME codepage COMMAND codepage.txt ${STDIO_ROM})

rp6502_add_script_test(NAME console COMMAND console.txt ${STDIO_ROM})

rp6502_add_script_test(NAME eof COMMAND eof.txt ${STDIO_ROM})

rp6502_add_script_test(NAME ctrl COMMAND ctrl.txt ${TTY_KEYS_ROM})

rp6502_add_script_test(NAME repeat COMMAND repeat.py)

rp6502_add_script_test(NAME zero COMMAND zero.txt ${STDIO_ROM})

rp6502_add_script_test(NAME registers COMMAND registers.txt ${ADVENTURE_ROM})

rp6502_add_script_test(NAME unassigned COMMAND unassigned.txt ${ADVENTURE_ROM})
set_tests_properties(script.unassigned PROPERTIES PASS_REGULAR_EXPRESSION
    "[$]FF00 is unassigned")

# The run fails on purpose, and the test checks the failure and the status
# line after it.
rp6502_add_script_test(NAME stuck COMMAND stuck.txt ${ADVENTURE_ROM})
set_tests_properties(script.stuck PROPERTIES PASS_REGULAR_EXPRESSION
    "timed out waiting for \"standing at the end of a road\"\nrp6502-emu: PC=[$][0-9A-F]+ A=[$][0-9A-F]+ X=[$][0-9A-F]+ Y=[$][0-9A-F]+ SP=[$][0-9A-F]+ P=[$][0-9A-F]+ OP=[$]16 BUSY=[$]FE RESB=1")

# The symbols tests use two copies of adventure, each with a hand-written .sym
# file, so a script can load the other copy and check that the symbols change.
foreach(_rom adventure other)
    configure_file(${ADVENTURE_ROM} ${CMAKE_CURRENT_BINARY_DIR}/symbols/${_rom}.rp6502 COPYONLY)
    configure_file(symbols/${_rom}.sym ${CMAKE_CURRENT_BINARY_DIR}/symbols/${_rom}.sym COPYONLY)
endforeach()
set(_symbols --install ${CMAKE_CURRENT_BINARY_DIR}/symbols/other.rp6502
    ${CMAKE_CURRENT_BINARY_DIR}/symbols/adventure.rp6502)
rp6502_add_script_test(NAME symbols COMMAND symbols.txt ${_symbols})
rp6502_add_script_test(NAME symbols_fail COMMAND symbols_fail.py ${_symbols})
rp6502_add_script_test(NAME symbols_keys COMMAND symbols_keys.txt ${KEYBOARD_ROM})

function(rp6502_add_cli_test name)
    add_test(NAME cli_${name} COMMAND rp6502-emu ${ARGN})
    set_tests_properties(cli_${name} PROPERTIES
        TIMEOUT 30 LABELS "cli;host.emu")
endfunction()

rp6502_add_cli_test(help --help)

foreach(_case
    "memseed;--memseed;x"
    "memseed_range;--memseed;4294967296"
    "memfill_random;--memfill;random"
    "memseed_and_memfill;--memseed;1;--memfill;0"
    "phi2;--phi2;fast"
    "scale;--scale;abc"
    "cp;--cp;4o7"
    "frames_without_screenshot;--frames;10"
    "headless_with_script;--headless;--script;-"
    "headless_with_crc;--headless;--crc"
    "aspect;--aspect;5:4"
    "aspect_empty;--aspect=")
    list(POP_FRONT _case _name)
    rp6502_add_cli_test(${_name} ${_case} ${RP6502_TEST_CORPUS}/mode3_8bpp.rp6502)
    set_tests_properties(cli_${_name} PROPERTIES WILL_FAIL TRUE)
endforeach()

# CMake regular expressions have no {n} repetition, so the CRC's eight hex
# digits are written out as eight character classes.
rp6502_add_cli_test(crc --crc --frames 10
    ${RP6502_TEST_CORPUS}/mode3_8bpp.rp6502)
set_tests_properties(cli_crc PROPERTIES PASS_REGULAR_EXPRESSION
    "^[0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F]\n")
rp6502_add_cli_test(phi2_zero --phi2 0 --crc --frames 1
    ${RP6502_TEST_CORPUS}/mode3_8bpp.rp6502)

# --aspect 16:9 makes the console 640x360, with 22 rows of 80 columns centered
# at lines 4 to 356. mode0_return ends on the console with its text there and
# the cursor off, so the frame is the same on every run and its CRC is pinned.
rp6502_add_cli_test(aspect_crc --aspect 16:9 --crc --frames 10
    ${RP6502_TEST_CORPUS}/mode0_return.rp6502)
set_tests_properties(cli_aspect_crc PROPERTIES PASS_REGULAR_EXPRESSION
    "^4E5A97C7\n")

add_test(NAME cli_fill_is_decimal COMMAND rp6502-emu
    --memfill 010
    --script ${CMAKE_CURRENT_LIST_DIR}/fill.txt
    ${RP6502_TEST_CORPUS}/mode3_8bpp.rp6502)
set_tests_properties(cli_fill_is_decimal PROPERTIES LABELS "cli;host.emu")

rp6502_add_test(units
    SOURCES test_units.c ${RP6502_SRC}/host/sokol/cli/cli.c
    LIBS emu_core ROM ${ADVENTURE_ROM})
if(WIN32)
    target_link_libraries(test_units PRIVATE wingetopt)
    target_compile_definitions(test_units PRIVATE _BSD_SOURCE)
endif()

rp6502_add_test(sys LIBS emu_core)

rp6502_add_test(sst LIBS emu_core ROM ${ADVENTURE_ROM})

# The state_inventory test runs only with GCC because Clang can merge
# file-scope statics into one MergedGlobals symbol, which leaves nothing to
# count.
if(CMAKE_EXPORT_COMPILE_COMMANDS
   AND CMAKE_C_COMPILER_ID STREQUAL "GNU"
   AND NOT APPLE AND NOT WIN32)
    add_test(NAME state_inventory
        COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_LIST_DIR}/state_inventory.py
            ${CMAKE_BINARY_DIR}/compile_commands.json
            ${RP6502_SRC}
            ${CMAKE_CURRENT_LIST_DIR}/state_inventory.txt)
    set_tests_properties(state_inventory PROPERTIES LABELS "gate;host.emu")
endif()

rp6502_add_test(roster INCLUDES ${CMAKE_CURRENT_LIST_DIR}/roster)

rp6502_add_test(com LIBS emu_core)

rp6502_add_test(rln LIBS emu_core)

rp6502_add_test(clk LIBS emu_core)

rp6502_add_test(attr LIBS emu_core)

rp6502_add_test(log SINK
    DEFS RP6502_LOG_LEVEL=RP6502_LOG_WARN RP6502_LOG_LEVEL_loud=RP6502_LOG_DEBUG)

rp6502_add_test(render LIBS emu_core ROM ${ADVENTURE_ROM})

rp6502_add_test(gamepad_input LIBS emu_core
    SOURCES test_gamepad_input.c ${RP6502_SRC}/host/sokol/app/gamepad.c)

rp6502_add_test(dbg LIBS emu_core ROM ${ADVENTURE_ROM} TIMEOUT 60)

rp6502_add_test(dwarf5
    SOURCES test_dwarf5.c
        ${RP6502_SRC}/core/dap/dwarf_info.c ${RP6502_SRC}/core/dap/dwarf_line.c
        ${RP6502_SRC}/core/dap/dwarf_cursor.c ${RP6502_SRC}/core/dap/dwarf_elf.c
    INCLUDES ${RP6502_SRC} ROM host/emu/dwtest.elf)

rp6502_add_test(dwarf_frame
    SOURCES test_dwarf_frame.c
        ${RP6502_SRC}/core/dap/dwarf_frame.c ${RP6502_SRC}/core/dap/dwarf_cursor.c
        ${RP6502_SRC}/core/dap/dwarf_elf.c
    INCLUDES ${RP6502_SRC} ROM host/emu/dwtest.elf)

rp6502_add_test(cc65dbg
    SOURCES test_cc65dbg.c ${RP6502_SRC}/core/dap/cc65dbg.c
    INCLUDES ${RP6502_SRC} ROM host/emu/cc65.dbg
    DEFS TEST_ONCE_FIXTURE="${CMAKE_CURRENT_LIST_DIR}/cc65_once.dbg")

rp6502_add_test(w65c02dasm INCLUDES ${RP6502_SRC} ${RP6502_VENDOR})

set(EXPR_CASES ${CMAKE_CURRENT_BINARY_DIR}/expr_cases.txt)
add_custom_command(OUTPUT ${EXPR_CASES}
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${CMAKE_CURRENT_LIST_DIR}/expr_cases.py --emit ${EXPR_CASES}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/expr_cases.py
    COMMENT "Generating the expression test cases"
    VERBATIM)
add_custom_target(expr_cases DEPENDS ${EXPR_CASES})
rp6502_add_test(expr ROM ${EXPR_CASES} DEPENDS expr_cases
    SOURCES test_expr.c ${RP6502_SRC}/host/sokol/cli/expr.c)
