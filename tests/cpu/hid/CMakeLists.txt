rp6502_add_test(gamepad LIBS emu_core TIMEOUT 60)

if(TARGET rp6502-emu)
    rp6502_add_script_test(gamepad ROM ${GAMEPAD_ROM} DEPENDS gamepad_rom)

    rp6502_add_script_test(keyboard ROM ${KEYBOARD_ROM} DEPENDS keyboard_rom)

    rp6502_add_script_test(pointer ROM ${MOUSE_ROM} DEPENDS pointer_roms)

    rp6502_add_script_test(tablet ROM ${TABLET_ROM} DEPENDS pointer_roms)

    rp6502_add_script_test(unmapped ROM ${RP6502_TEST_CORPUS}/mode3_8bpp.rp6502
        DEPENDS rp6502_test_corpus)
endif()
