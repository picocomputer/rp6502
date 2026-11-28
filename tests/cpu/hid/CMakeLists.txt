rp6502_add_test(gamepad LIBS emu_core TIMEOUT 60)
rp6502_add_test(keyboard LIBS emu_core TIMEOUT 60)

# keymap.c, the keyboard driver of a machine with no system under it, on
# emu_core, which has hid_null.c in its place; the test keeps that out of the
# link by defining what else it holds. emu_core's machine has no layout
# driver, so layout.c and its database are built as the Pocket, which has
# one, builds them.
set_source_files_properties(${RP6502_GEN}/layout_db.c PROPERTIES GENERATED TRUE)
add_library(test_keymap_layout OBJECT
    ${RP6502_SRC}/core/hid/layout.c ${RP6502_GEN}/layout_db.c)
target_include_directories(test_keymap_layout PRIVATE
    ${RP6502_SRC}/host/pocket ${RP6502_SRC} ${RP6502_GEN})
target_compile_definitions(test_keymap_layout PRIVATE
    PICO_PROGRAM_NAME="RP6502-FPGA" RP6502_LOCALE=EN RP6502_EXFAT=0)
if(COMMAND rp6502_msvc_dialect)
    rp6502_msvc_dialect(test_keymap_layout)
endif()
add_dependencies(test_keymap_layout rp6502_gen_c)
rp6502_add_test(keymap
    SOURCES test_keymap.c ${RP6502_SRC}/core/hid/keymap.c
        $<TARGET_OBJECTS:test_keymap_layout>
    LIBS emu_core TIMEOUT 60)

rp6502_add_test(parse SOURCES test_parse.c ${RP6502_SRC}/core/hid/parse.c
    LIBS emu_core TIMEOUT 60)

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
