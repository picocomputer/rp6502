# The shipped emulator, driven the way an application developer drives it: a
# --script file, an exit code, and nothing else installed. These are the only
# tests that execute rp6502-emu itself rather than linking emu_core, so they
# are also the only ones that cover cli.c and main.c.
#
# The scripts that pair with a subsystem live with it — the pointer's beside
# the C test that makes the same claim about the pointer. What is left here is
# what is about the binary rather than about the machine.

# The exception: how many frames a script spends is the one thing a script
# cannot check about itself, so that one is a C test around scr.c.
rp6502_add_test(script LIBS emu_core FIXTURE gamepad.rp6502
    SOURCES test_script.c ${RP6502_SRC}/core/emu/app/scr.c ${RP6502_SRC}/core/emu/app/png.c
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
