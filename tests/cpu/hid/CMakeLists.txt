rp6502_add_test(gamepad LIBS emu_core TIMEOUT 60)

if(TARGET rp6502-emu)
    rp6502_add_script_test(NAME gamepad COMMAND gamepad.py)
    rp6502_add_script_test(NAME gamepad_bounds COMMAND gamepad.py)
    rp6502_add_script_test(NAME gamepad_held COMMAND gamepad.py)
    rp6502_add_script_test(NAME keyboard COMMAND keyboard.py)
    rp6502_add_script_test(NAME pointer COMMAND pointer.py)
    rp6502_add_script_test(NAME tablet COMMAND pointer.py)
    rp6502_add_script_test(NAME unmapped COMMAND unmapped.txt
        ${RP6502_TEST_CORPUS}/mode3_8bpp.rp6502)
endif()
