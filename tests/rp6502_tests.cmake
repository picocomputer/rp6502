include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/utest SENTINEL utest.h
    WANTS "the test harness")

set(RP6502_UTEST_DIR ${RP6502_VENDOR}/utest)
set(RP6502_TESTS_DIR ${CMAKE_CURRENT_LIST_DIR})
set(RP6502_TEST_ROMS ${RP6502_TESTS_DIR}/roms)

set(ADVENTURE_ROM ${RP6502_TEST_ROMS}/adventure.rp6502)

if(WIN32)
    set(RP6502_TB_HOSTOS ${CMAKE_CURRENT_LIST_DIR}/bench/tb_hostos_win.c)
else()
    set(RP6502_TB_HOSTOS ${CMAKE_CURRENT_LIST_DIR}/bench/tb_hostos_posix.c)
endif()

set(RP6502_BENCH ${RP6502_TESTS_DIR}/bench)

include(${RP6502_SRC}/core/gen.cmake)
include(${RP6502_SRC}/core/log.cmake)
rp6502_gen_kbdlay(kbdlay)

set(RP6502_TEST_ROM_DIR ${CMAKE_BINARY_DIR}/roms)
set(RP6502_TEST_CORPUS ${RP6502_TEST_ROM_DIR})
if(NOT TARGET rp6502_test_corpus)
    set(RP6502_CORPUS_GEN ${RP6502_TESTS_DIR}/gen/vidmodes.py)
    set(RP6502_CORPUS_ASM
        ${RP6502_TESTS_DIR}/gen/rp6502_asm.py
        ${RP6502_TESTS_DIR}/gen/rp6502_rom.py)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/roms.stamp
        COMMAND ${CMAKE_COMMAND} -E env python3
            ${RP6502_CORPUS_GEN} --out ${RP6502_TEST_CORPUS}
            --emit-manifest ${RP6502_TEST_CORPUS}/manifest.txt
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/roms.stamp
        DEPENDS ${RP6502_CORPUS_GEN} ${RP6502_CORPUS_ASM}
        COMMENT "Generating the video-mode ROM corpus"
        VERBATIM)
    add_custom_target(rp6502_test_corpus DEPENDS ${CMAKE_BINARY_DIR}/roms.stamp)
endif()

# add_test cannot depend on a target and a script test has no executable of
# its own, so the ROMs are built as part of ALL.
if(NOT TARGET rp6502_test_roms)
    add_custom_target(rp6502_test_roms ALL)
    add_dependencies(rp6502_test_roms rp6502_test_corpus)
endif()

function(rp6502_test_rom target)
    cmake_parse_arguments(R "" "GEN;COMMENT" "OUTPUTS;ARGS;DEPENDS" ${ARGN})
    add_custom_command(OUTPUT ${R_OUTPUTS}
        COMMAND ${CMAKE_COMMAND} -E env python3 ${R_GEN} ${R_ARGS}
        DEPENDS ${R_GEN} ${R_DEPENDS}
        COMMENT ${R_COMMENT}
        VERBATIM)
    add_custom_target(${target} DEPENDS ${R_OUTPUTS})
    add_dependencies(rp6502_test_roms ${target})
endfunction()

set(RP6502_ROM_GEN
    ${RP6502_TESTS_DIR}/gen/rp6502_asm.py
    ${RP6502_TESTS_DIR}/gen/rp6502_rom.py)

set(AUD_ROM_PSG ${RP6502_TEST_ROM_DIR}/psg.rp6502)
set(AUD_ROM_PSG_PRE ${RP6502_TEST_ROM_DIR}/psg_pre.rp6502)
set(AUD_ROM_OPL ${RP6502_TEST_ROM_DIR}/opl.rp6502)
set(AUD_ROM_OPL_EXIT ${RP6502_TEST_ROM_DIR}/opl_exit.rp6502)
set(AUD_ROM_OPL_INIT ${RP6502_TEST_ROM_DIR}/opl_init.rp6502)
set(AUD_ROM_BEL ${RP6502_TEST_ROM_DIR}/bel.rp6502)
set(AUD_ROM_OPL_BEL ${RP6502_TEST_ROM_DIR}/opl_bel.rp6502)
rp6502_test_rom(aud_roms GEN ${RP6502_TESTS_DIR}/gen/aud_rom_gen.py
    ARGS --emit-psg ${AUD_ROM_PSG} --emit-psg-pre ${AUD_ROM_PSG_PRE}
        --emit-opl ${AUD_ROM_OPL}
        --emit-opl-exit ${AUD_ROM_OPL_EXIT}
        --emit-opl-init ${AUD_ROM_OPL_INIT}
        --emit-bel ${AUD_ROM_BEL} --emit-opl-bel ${AUD_ROM_OPL_BEL}
    OUTPUTS ${AUD_ROM_PSG} ${AUD_ROM_PSG_PRE} ${AUD_ROM_OPL}
        ${AUD_ROM_OPL_EXIT} ${AUD_ROM_OPL_INIT}
        ${AUD_ROM_BEL} ${AUD_ROM_OPL_BEL}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the audio bring-up ROMs")

set(STREAM_ROM ${RP6502_TEST_ROM_DIR}/stream.rp6502)
rp6502_test_rom(stream_rom GEN ${RP6502_TESTS_DIR}/gen/stream_rom_gen.py
    ARGS --emit ${STREAM_ROM}
    OUTPUTS ${STREAM_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the streaming-read ROM")

set(FILE_ROM ${RP6502_TEST_ROM_DIR}/file.rp6502)
rp6502_test_rom(file_rom GEN ${RP6502_TESTS_DIR}/gen/file_rom_gen.py
    ARGS --emit ${FILE_ROM}
    OUTPUTS ${FILE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the file round-trip ROM")

set(BIGFILE_ROM ${RP6502_TEST_ROM_DIR}/bigfile.rp6502)
rp6502_test_rom(bigfile_rom GEN ${RP6502_TESTS_DIR}/gen/bigfile_rom_gen.py
    ARGS --emit ${BIGFILE_ROM}
    OUTPUTS ${BIGFILE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the multi-chunk file ROM")

set(PROBE_ROM ${RP6502_TEST_ROM_DIR}/probe.rp6502)
rp6502_test_rom(probe_rom GEN ${RP6502_TESTS_DIR}/gen/probe_rom_gen.py
    ARGS --emit ${PROBE_ROM}
    OUTPUTS ${PROBE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the open-file probe ROM")

set(SLEEPFILE_ROM ${RP6502_TEST_ROM_DIR}/sleepfile.rp6502)
rp6502_test_rom(sleepfile_rom GEN ${RP6502_TESTS_DIR}/gen/sleepfile_rom_gen.py
    ARGS --emit ${SLEEPFILE_ROM}
    OUTPUTS ${SLEEPFILE_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the sleep file probe ROM")

set(SLEEPASSET_A_ROM ${RP6502_TEST_ROM_DIR}/sleepasset-a.rp6502)
set(SLEEPASSET_B_ROM ${RP6502_TEST_ROM_DIR}/sleepasset-b.rp6502)
rp6502_test_rom(sleepasset_a_rom GEN ${RP6502_TESTS_DIR}/gen/sleepasset_rom_gen.py
    ARGS --emit ${SLEEPASSET_A_ROM} --variant A
    OUTPUTS ${SLEEPASSET_A_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the sleep asset probe ROM (A)")
rp6502_test_rom(sleepasset_b_rom GEN ${RP6502_TESTS_DIR}/gen/sleepasset_rom_gen.py
    ARGS --emit ${SLEEPASSET_B_ROM} --variant B
    OUTPUTS ${SLEEPASSET_B_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the sleep asset probe ROM (B)")

set(ARGV_ROM ${RP6502_TEST_ROM_DIR}/argv.rp6502)
rp6502_test_rom(argv_rom GEN ${RP6502_TESTS_DIR}/gen/argv_rom_gen.py
    ARGS --emit ${ARGV_ROM}
    OUTPUTS ${ARGV_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the argv ROM")

set(EXEC_ROM ${RP6502_TEST_ROM_DIR}/exec.rp6502)
rp6502_test_rom(exec_rom GEN ${RP6502_TESTS_DIR}/gen/exec_rom_gen.py
    ARGS --emit ${EXEC_ROM}
    OUTPUTS ${EXEC_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the exec ROM")

set(TIME_ROM ${RP6502_TEST_ROM_DIR}/time.rp6502)
rp6502_test_rom(time_rom GEN ${RP6502_TESTS_DIR}/gen/time_rom_gen.py
    ARGS --emit ${TIME_ROM}
    OUTPUTS ${TIME_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the time ROM")

set(FSTEST_ROM ${RP6502_TEST_ROM_DIR}/fstest.rp6502)
rp6502_test_rom(fstest_rom GEN ${RP6502_TESTS_DIR}/gen/fstest_rom_gen.py
    ARGS --emit ${FSTEST_ROM}
    OUTPUTS ${FSTEST_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the filesystem conformance ROM")

set(DIR_ROM ${RP6502_TEST_ROM_DIR}/dir.rp6502)
rp6502_test_rom(dir_rom GEN ${RP6502_TESTS_DIR}/gen/dir_rom_gen.py
    ARGS --emit ${DIR_ROM}
    OUTPUTS ${DIR_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the directory listing ROM")

set(TTY_ROM ${RP6502_TEST_ROM_DIR}/tty.rp6502)
rp6502_test_rom(tty_rom GEN ${RP6502_TESTS_DIR}/gen/tty_rom_gen.py
    ARGS --emit ${TTY_ROM}
    OUTPUTS ${TTY_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the raw console read ROM")

set(CON_ROM ${RP6502_TEST_ROM_DIR}/con.rp6502)
rp6502_test_rom(con_rom GEN ${RP6502_TESTS_DIR}/gen/con_rom_gen.py
    ARGS --emit ${CON_ROM}
    OUTPUTS ${CON_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the console device ROM")

set(STDIO_ROM ${RP6502_TEST_ROM_DIR}/stdio.rp6502)
rp6502_test_rom(stdio_rom GEN ${RP6502_TESTS_DIR}/gen/stdio_rom_gen.py
    ARGS --emit ${STDIO_ROM}
    OUTPUTS ${STDIO_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the standard streams ROM")

set(KEYBOARD_ROM ${RP6502_TEST_ROM_DIR}/keyboard.rp6502)
rp6502_test_rom(keyboard_rom GEN ${RP6502_TESTS_DIR}/gen/keyboard_rom_gen.py
    ARGS --emit ${KEYBOARD_ROM}
    OUTPUTS ${KEYBOARD_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the keyboard bitmap ROM")

set(MOUSE_ROM ${RP6502_TEST_ROM_DIR}/mouse.rp6502)
set(TABLET_ROM ${RP6502_TEST_ROM_DIR}/tablet.rp6502)
rp6502_test_rom(pointer_roms GEN ${RP6502_TESTS_DIR}/gen/pointer_rom_gen.py
    ARGS --emit-mouse ${MOUSE_ROM} --emit-tablet ${TABLET_ROM}
    OUTPUTS ${MOUSE_ROM} ${TABLET_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the pointing device ROMs")

set(GAMEPAD_ROM ${RP6502_TEST_ROM_DIR}/gamepad.rp6502)
rp6502_test_rom(gamepad_rom GEN ${RP6502_TESTS_DIR}/gen/gamepad_rom_gen.py
    ARGS --emit ${GAMEPAD_ROM}
    OUTPUTS ${GAMEPAD_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the gamepad records ROM")

set(VSYNC_ROM ${RP6502_TEST_ROM_DIR}/vsync.rp6502)
rp6502_test_rom(vsync_rom GEN ${RP6502_TESTS_DIR}/gen/vsync_rom_gen.py
    ARGS --emit ${VSYNC_ROM}
    OUTPUTS ${VSYNC_ROM}
    DEPENDS ${RP6502_ROM_GEN}
    COMMENT "Generating the vsync interrupt ROM")

function(rp6502_add_script_test name)
    cmake_parse_arguments(S "" "SCRIPT;DRIVER;ROM;TIMEOUT" "ARGS;DEPENDS" ${ARGN})
    if(NOT TARGET rp6502-emu)
        message(FATAL_ERROR
            "rp6502_add_script_test(${name}) drives the shipped binary.\n"
            "  Guard the call with if(TARGET rp6502-emu).")
    endif()

    if(S_DRIVER)
        if(NOT IS_ABSOLUTE ${S_DRIVER})
            set(S_DRIVER ${CMAKE_CURRENT_LIST_DIR}/${S_DRIVER})
        endif()
        set(S_SCRIPT ${S_DRIVER})
    else()
        if(NOT S_SCRIPT)
            set(S_SCRIPT ${name}.txt)
        endif()
        if(NOT IS_ABSOLUTE ${S_SCRIPT})
            set(S_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/${S_SCRIPT})
        endif()
    endif()
    if(NOT S_ROM)
        message(FATAL_ERROR "rp6502_add_script_test(${name}) names no program")
    endif()
    # A relative path in a script's shot command or in a program's file calls
    # resolves against the working directory, so each script test gets its own
    # directory and tests run by ctest --parallel do not overwrite each other's
    # files. The same directory is the SAVE: folder, so no test writes into
    # the save folder of the user who runs it.
    set(_work ${CMAKE_CURRENT_BINARY_DIR}/script.${name})
    file(MAKE_DIRECTORY ${_work})

    file(RELATIVE_PATH _dir ${RP6502_TESTS_DIR} ${CMAKE_CURRENT_LIST_DIR})
    string(REPLACE "/" "." _dir "${_dir}")

    if(S_DRIVER)
        add_test(NAME script.${name}
            COMMAND ${CMAKE_COMMAND} -E env python3 ${S_DRIVER} --drive
                --emu $<TARGET_FILE:rp6502-emu> --rom ${S_ROM}
                --save-dir ${_work} ${S_ARGS})
    else()
        add_test(NAME script.${name}
            COMMAND rp6502-emu --mute --seed 1 --fill 0 --save-dir ${_work}
                ${S_ARGS} --script ${S_SCRIPT} ${S_ROM})
    endif()
    if(NOT S_TIMEOUT)
        set(S_TIMEOUT 120)
    endif()
    set_tests_properties(script.${name} PROPERTIES
        WORKING_DIRECTORY ${_work}
        TIMEOUT ${S_TIMEOUT}
        ENVIRONMENT EMU_ECHO=1
        LABELS "script;${_dir}")

    if(S_DEPENDS)
        add_dependencies(rp6502_test_roms ${S_DEPENDS})
    endif()
endfunction()

function(rp6502_add_test name)
    cmake_parse_arguments(T "SPLIT;SINK" "ROM;TIMEOUT"
        "SOURCES;LIBS;INCLUDES;DEFS;LABELS;DEPENDS" ${ARGN})

    file(RELATIVE_PATH _dir ${RP6502_TESTS_DIR} ${CMAKE_CURRENT_LIST_DIR})
    string(REPLACE "/" "." _dir "${_dir}")
    if(NOT T_LABELS)
        set(T_LABELS c)
    endif()
    list(APPEND T_LABELS ${_dir})

    if(NOT T_SOURCES)
        set(T_SOURCES test_${name}.c)
    endif()

    list(APPEND T_SOURCES ${RP6502_BENCH}/tb_seed.c ${RP6502_SRC}/core/sys/crc32.c)
    if(NOT T_SINK)
        list(APPEND T_SOURCES ${RP6502_BENCH}/tb_log.c)
    endif()

    add_executable(test_${name} ${T_SOURCES})
    target_include_directories(test_${name} PRIVATE
        ${RP6502_UTEST_DIR} ${RP6502_BENCH} ${RP6502_SRC} ${T_INCLUDES})

    if(T_LIBS)
        target_link_libraries(test_${name} PRIVATE ${T_LIBS})
    endif()
    list(APPEND T_DEFS TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}")
    if(T_ROM)
        if(NOT IS_ABSOLUTE ${T_ROM})
            set(T_ROM ${RP6502_TESTS_DIR}/${T_ROM})
        endif()
        list(APPEND T_DEFS TEST_FIXTURE="${T_ROM}")
    endif()
    if(T_DEFS)
        target_compile_definitions(test_${name} PRIVATE ${T_DEFS})
    endif()
    if(NOT "emu_core" IN_LIST T_LIBS AND NOT "${T_DEFS}" MATCHES "RP6502_LOG_LEVEL=")
        rp6502_log_definitions(test_${name} PRIVATE)
    endif()

    add_dependencies(test_${name} rp6502_test_corpus ${T_DEPENDS})

    set(_cases ${name})
    if(T_SPLIT)
        list(GET T_SOURCES 0 _src)
        if(NOT IS_ABSOLUTE ${_src})
            set(_src ${CMAKE_CURRENT_LIST_DIR}/${_src})
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_src})
        set(_cases)
        file(STRINGS ${_src} _decls REGEX "^UTEST\\(")
        foreach(_decl IN LISTS _decls)
            string(REGEX REPLACE "^UTEST\\( *([A-Za-z0-9_]+) *, *([A-Za-z0-9_]+).*"
                "\\1.\\2" _case "${_decl}")
            list(APPEND _cases ${_case})
        endforeach()
        if(NOT _cases)
            message(FATAL_ERROR "${_src} declares no UTEST cases to split")
        endif()
    endif()

    foreach(_case IN LISTS _cases)
        set(_filter)
        if(T_SPLIT)
            set(_filter --filter=${_case})
        endif()
        add_test(NAME ${_case} COMMAND test_${name} ${_filter})
        set_tests_properties(${_case} PROPERTIES LABELS "${T_LABELS}")
        if(T_TIMEOUT)
            set_tests_properties(${_case} PROPERTIES TIMEOUT ${T_TIMEOUT})
        endif()
    endforeach()
    if(T_SPLIT)
        # utest exits with zero when --filter matches no case, so without this
        # regular expression a case that never ran would pass.
        set_tests_properties(${_cases} PROPERTIES
            FAIL_REGULAR_EXPRESSION "Running 0 test cases")
    endif()
endfunction()
