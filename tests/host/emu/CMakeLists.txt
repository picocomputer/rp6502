# The shipped emulator, driven the way an application developer drives it: a
# --script file, an exit code, and nothing else installed. These are the only
# tests that execute rp6502-emu itself rather than linking emu_core, so they
# are also the only ones that cover cli.c and main.c.
#
# rp6502_add_test builds a C executable from test_<name>.c and has nothing to
# offer the script files, so those are plain add_test lines.

# The exception: how many frames a script spends is the one thing a script
# cannot check about itself, so that one is a C test around scr.c.
rp6502_add_test(script LIBS emu_core FIXTURE gamepad.rp6502
    SOURCES test_script.c ${RP6502_SRC}/emu/app/scr.c ${RP6502_SRC}/emu/app/png.c
    TIMEOUT 60)

# --mute so a CI runner opens no audio device, --seed so a program that asks
# for entropy asks for the same entropy every run.
function(rp6502_add_script_test name rom)
    add_test(NAME emu_script_${name} COMMAND rp6502-emu
        --mute --seed 1
        --script ${CMAKE_CURRENT_LIST_DIR}/${name}.txt
        ${RP6502_TEST_ROMS}/${rom})
    set_tests_properties(emu_script_${name} PROPERTIES TIMEOUT 120)
endfunction()

# Gamepad: the connected gate and the four-player XRAM block (pad.c).
rp6502_add_script_test(gamepad gamepad.rp6502)

# Direct keyboard: the HID bitmap a program polls, not the stdin stream.
rp6502_add_script_test(keyboard mode2.rp6502)

# Relative mouse, through the 6522 timer IRQ the paint demo runs its ISR on.
rp6502_add_script_test(pointer paint_mou.rp6502)

# Absolute pointer: the window-encoded coordinates in the tablet block.
rp6502_add_script_test(tablet paint_tab.rp6502)

# The ANSI keyboard: type and wait against a program that reads whole lines.
rp6502_add_script_test(adventure adventure.rp6502)

# Writing memory the program reads, rather than driving it through the inputs.
rp6502_add_script_test(poke gamepad.rp6502)
