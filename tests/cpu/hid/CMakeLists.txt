# Pointers and gamepads, each driven by the SDK demo that exercises it.

# --- 6522 VIA timer IRQ + HID mouse, via the paint_mouse demo ---
rp6502_add_test(paint_mouse LIBS emu_core FIXTURE paint_mouse.rp6502 TIMEOUT 60)

# --- Absolute pointer (tablet) window encoding, via paint_tablet ---
rp6502_add_test(paint_tablet LIBS emu_core FIXTURE paint_tablet.rp6502 TIMEOUT 60)

# --- Gamepad (HID gamepad xreg + gamepad_xram_t mirror), via the gamepad tester ---
rp6502_add_test(gamepad LIBS emu_core FIXTURE gamepad.rp6502 TIMEOUT 60)

# The same claims through the shipped binary, driven the way a developer
# drives it. The C tests above reach APIs no script can — gamepad_xreg's bounds,
# the unmapping — and these reach cli.c, main.c and script.c, which no C test
# does. Each sits beside the one it overlaps rather than in a directory about
# the host, so "how is the pointer proven" has one place to look.
if(TARGET rp6502-emu)
    # The connected gate, the four-player XRAM block, and the status nibble.
    rp6502_add_script_test(gamepad FIXTURE gamepad.rp6502)

    # The HID bitmap a program polls, not the stdin stream.
    rp6502_add_script_test(keyboard FIXTURE mode2.rp6502)

    # Relative mouse, through the 6522 timer IRQ the paint demo runs its ISR on.
    rp6502_add_script_test(pointer FIXTURE paint_mouse.rp6502)

    # Absolute pointer: the window-encoded coordinates in the tablet block.
    rp6502_add_script_test(tablet FIXTURE paint_tablet.rp6502)

    # And the other half of every one of those: a device nobody mapped is
    # silent. Each block's address register means nowhere until a program says
    # otherwise, and nowhere is the one address a driver cannot be allowed to
    # guess -- zero is XRAM a program is already using.
    rp6502_add_script_test(unmapped FIXTURE mode2.rp6502)
endif()
