rp6502_add_test(script LIBS emu_core ROM ${GAMEPAD_ROM} DEPENDS gamepad_rom
    SOURCES test_script.c ${RP6502_SRC}/host/sokol/cli/script.c
        ${RP6502_SRC}/host/sokol/cli/png.c ${RP6502_SRC}/host/sokol/cli/streams.c
        ${RP6502_SRC}/host/sokol/cli/state.c
    TIMEOUT 60)

rp6502_add_script_test(poke ROM ${GAMEPAD_ROM} DEPENDS gamepad_rom)

rp6502_add_script_test(state ROM ${GAMEPAD_ROM} DEPENDS gamepad_rom)

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
    "frames_without_screenshot;--frames;10"
    "headless_with_script;--headless;--script;-"
    "headless_with_crc;--headless;--crc")
    list(POP_FRONT _case _name)
    rp6502_add_cli_test(${_name} ${_case} ${RP6502_TEST_CORPUS}/mode3_8bpp.rp6502)
    set_tests_properties(cli_${_name} PROPERTIES WILL_FAIL TRUE)
endforeach()

# CMake regular expressions have no {n} repetition, so the CRC's eight hex
# digits are written out as eight character classes.
rp6502_add_cli_test(crc --mute --seed 1 --fill 0 --crc --frames 10
    ${RP6502_TEST_CORPUS}/mode3_8bpp.rp6502)
set_tests_properties(cli_crc PROPERTIES PASS_REGULAR_EXPRESSION
    "^[0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F]\n")
rp6502_add_cli_test(phi2_zero --mute --seed 1 --fill 0 --phi2 0 --crc --frames 1
    ${RP6502_TEST_CORPUS}/mode3_8bpp.rp6502)

add_test(NAME cli_fill_is_decimal COMMAND rp6502-emu
    --mute --seed 1 --fill 010
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
            ${RP6502_TESTS_DIR}/gen/state_inventory.py
            ${CMAKE_BINARY_DIR}/compile_commands.json
            ${RP6502_SRC}
            ${RP6502_TESTS_DIR}/gen/state_inventory.txt)
    set_tests_properties(state_inventory PROPERTIES LABELS "gate;host.emu")
endif()

rp6502_add_test(roster INCLUDES ${CMAKE_CURRENT_LIST_DIR}/roster)

rp6502_add_test(com LIBS emu_core)

rp6502_add_test(rln LIBS emu_core)

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
    INCLUDES ${RP6502_SRC} ROM host/emu/cc65.dbg)

rp6502_add_test(w65c02dasm INCLUDES ${RP6502_SRC} ${RP6502_VENDOR})
