if(NOT TARGET rp6502_libretro)
    return()
endif()

set(RP6502_RETRO_DEFS RETRO_SO="$<TARGET_FILE:rp6502_libretro>")

rp6502_add_test(abi
    SOURCES test_abi.c
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS}
    DEPENDS rp6502_libretro
    TIMEOUT 60)

rp6502_add_test(load
    SOURCES test_load.cpp
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS}
    DEPENDS rp6502_libretro
    TIMEOUT 60)

rp6502_add_test(run
    SOURCES test_run.c
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS} ROMS_DIR="${RP6502_TEST_CORPUS}"
        AUD_ROM_OPL="${AUD_ROM_OPL}"
        AUD_ROM_PSG="${AUD_ROM_PSG}"
    DEPENDS rp6502_libretro aud_roms
    TIMEOUT 60)

rp6502_add_test(input
    SOURCES test_input.c
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS} ADVENTURE_ROM="${ADVENTURE_ROM}"
        KEYBOARD_ROM="${KEYBOARD_ROM}"
        GAMEPAD_ROM="${GAMEPAD_ROM}"
        MOUSE_ROM="${MOUSE_ROM}" TABLET_ROM="${TABLET_ROM}"
    DEPENDS rp6502_libretro keyboard_rom gamepad_rom pointer_roms
    TIMEOUT 60)

rp6502_add_test(options
    SOURCES test_options.c
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS} ROMS_DIR="${RP6502_TEST_CORPUS}"
        GAMEPAD_ROM="${GAMEPAD_ROM}"
    DEPENDS rp6502_libretro gamepad_rom
    TIMEOUT 60)

rp6502_add_test(pointer
    SOURCES test_pointer.c
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS}
        MOUSE_ROM="${MOUSE_ROM}" TABLET_ROM="${TABLET_ROM}"
    DEPENDS rp6502_libretro pointer_roms
    TIMEOUT 60)

execute_process(COMMAND ${Python3_EXECUTABLE} -c "import yaml"
    RESULT_VARIABLE _have_yaml OUTPUT_QUIET ERROR_QUIET)
if(_have_yaml EQUAL 0)
    add_test(NAME libretro_targets
        COMMAND ${Python3_EXECUTABLE}
            ${RP6502_TESTS_DIR}/gen/libretro_targets_gate.py
            --gitlab ${RP6502_ROOT}/.gitlab-ci.yml
            --github ${RP6502_ROOT}/.github/workflows/ci.yml)
    set_tests_properties(libretro_targets PROPERTIES LABELS "gate;host.libretro")
endif()

rp6502_add_test(state
    SOURCES test_state.c
    LIBS ${CMAKE_DL_LIBS}
    INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
    DEFS ${RP6502_RETRO_DEFS} ADVENTURE_ROM="${ADVENTURE_ROM}"
        AUD_ROM_PSG="${AUD_ROM_PSG}"
    DEPENDS rp6502_libretro aud_roms
    TIMEOUT 120)
