# Pointers and pads, each driven by the SDK demo that exercises it.

# --- 6522 VIA timer IRQ + HID mouse, via the paint_mou demo ---
rp6502_add_test(paint_mou LIBS emu_core FIXTURE paint_mou.rp6502 TIMEOUT 60)

# --- Absolute pointer (tablet) window encoding, via paint_tab ---
rp6502_add_test(paint_tab LIBS emu_core FIXTURE paint_tab.rp6502 TIMEOUT 60)

# --- Gamepad (HID pad xreg + pad_xram_t mirror), via the gamepad tester ---
rp6502_add_test(gamepad LIBS emu_core FIXTURE gamepad.rp6502 TIMEOUT 60)

# --- The desktop gamepad policy, against a host that does as it is told ---
# Reading a controller is per-OS code no runner has hardware for. Everything
# above that seam is not, and the privacy gate and player numbering are the
# parts worth being sure of.
rp6502_add_test(pad_input LIBS emu_core
    SOURCES test_pad_input.c ${RP6502_SRC}/emu/app/pad_input.c)

# --- The Pocket's synthetic descriptors against the drivers they feed ---
# The dock's registers are handed to ria/hid as HID reports, and three
# descriptors written by hand are what says where every bit went. Wrong
# ones build clean and boot clean, so the registers go in here and the
# XRAM records come back out. The real drivers, not the emulator's
# twins: it is the firmware's mapping being asked about.
rp6502_add_test(apf
    SOURCES test_apf.c
        ${RP6502_SRC}/core/hid/hid.c
        ${RP6502_SRC}/core/hid/kbd.c
        ${RP6502_SRC}/core/hid/kbl.c
        ${RP6502_SRC}/core/hid/mou.c
        ${RP6502_SRC}/core/hid/pad.c
        ${RP6502_SRC}/core/hid/tab.c
        ${RP6502_SRC}/core/api/uni.c
        ${KBDLAY_C} ${OEMCP_C}
        apf_host.c
    INCLUDES ${RP6502_SRC}/host/pocket ${RP6502_SRC} ${RP6502_SRC}/pico_shim
        ${KBDLAY_DIR} ${OEMCP_DIR} ${RP6502_SRC}/rtl/sw/shim)
add_dependencies(test_apf kbdlay oemcp)
if(MSVC)
    # The shims emu_core hands the shared firmware sources, which this
    # test compiles without linking it: a <strings.h> MSVC has no system
    # header for, and the idioms it dislikes where GCC does not.
    target_include_directories(test_apf PRIVATE ${RP6502_SRC}/host/win)
    target_compile_options(test_apf PRIVATE /utf-8 /FIcompat.h /wd4068 /wd4098)
endif()

# The same claims through the shipped binary, driven the way a developer
# drives it. The C tests above reach APIs no script can — pad_set_xram's
# bounds, the unmapping — and these reach cli.c, main.c and scr.c, which no C
# test does. Each sits beside the one it overlaps rather than in a directory
# about the host, so "how is the pointer proven" has one place to look.
if(TARGET rp6502-emu)
    # The connected gate, the four-player XRAM block, and the status nibble.
    rp6502_add_script_test(gamepad FIXTURE gamepad.rp6502)

    # The HID bitmap a program polls, not the stdin stream.
    rp6502_add_script_test(keyboard FIXTURE mode2.rp6502)

    # Relative mouse, through the 6522 timer IRQ the paint demo runs its ISR on.
    rp6502_add_script_test(pointer FIXTURE paint_mou.rp6502)

    # Absolute pointer: the window-encoded coordinates in the tablet block.
    rp6502_add_script_test(tablet FIXTURE paint_tab.rp6502)
endif()
