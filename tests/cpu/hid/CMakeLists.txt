# Pointers, gamepads and the keyboard, each run by a program generated from
# source in tests/gen rather than a binary committed beside it. The C tests
# reach APIs no script can — gamepad_xreg's bounds, the unmapping — and the
# scripts reach cli.c, main.c and script.c, which no C test does. Each sits
# beside the one it overlaps rather than in a directory about the host, so
# "how is the pointer proven" has one place to look.

# --- Gamepad (HID gamepad xreg + gamepad_xram_t mirror) ---
rp6502_add_test(gamepad LIBS emu_core TIMEOUT 60)

if(TARGET rp6502-emu)
    # The connected gate, the four-player XRAM block, and the status nibble.
    rp6502_add_script_test(gamepad ROM ${GAMEPAD_ROM} DEPENDS gamepad_rom)

    # The HID bitmap a program polls, not the stdin stream.
    rp6502_add_script_test(keyboard ROM ${KEYBOARD_ROM} DEPENDS keyboard_rom)

    # Relative mouse, read back through a 6522 timer interrupt.
    rp6502_add_script_test(pointer ROM ${MOUSE_ROM} DEPENDS pointer_roms)

    # Absolute pointer: the window-encoded coordinates in the tablet block.
    rp6502_add_script_test(tablet ROM ${TABLET_ROM} DEPENDS pointer_roms)

    # And the other half of every one of those: a device nobody mapped is
    # silent. Each block's address register means nowhere until a program says
    # otherwise, and nowhere is the one address a driver cannot be allowed to
    # guess -- zero is XRAM a program is already using.
    rp6502_add_script_test(unmapped ROM ${RP6502_TEST_CORPUS}/mode3_8bpp.rp6502
        DEPENDS rp6502_test_corpus)
endif()
