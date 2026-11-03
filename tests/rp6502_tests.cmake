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

# The Python files of the tests import the assembler and the test runner
# from lib, and rp6502 from tools. PYTHONPATH separates folders with ; on
# Windows and with : on other hosts.
set(RP6502_TEST_LIB ${RP6502_TESTS_DIR}/lib)
if(CMAKE_HOST_WIN32)
    set(RP6502_TEST_PYTHONPATH "${RP6502_TEST_LIB}$<SEMICOLON>${RP6502_ROOT}/tools")
else()
    set(RP6502_TEST_PYTHONPATH "${RP6502_TEST_LIB}:${RP6502_ROOT}/tools")
endif()
set(RP6502_TEST_PYTHON ${CMAKE_COMMAND} -E env
    PYTHONPATH=${RP6502_TEST_PYTHONPATH} PYTHONDONTWRITEBYTECODE=1 python3)
set(RP6502_TEST_LIB_FILES
    ${RP6502_TEST_LIB}/rp6502_asm.py
    ${RP6502_TEST_LIB}/rp6502_rom.py
    ${RP6502_ROOT}/tools/rp6502.py)

if(NOT TARGET rp6502_test_corpus)
    set(RP6502_CORPUS_GEN ${RP6502_TESTS_DIR}/cpu/vga/vidmodes.py)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/roms.stamp
        COMMAND ${RP6502_TEST_PYTHON}
            ${RP6502_CORPUS_GEN} --out ${RP6502_TEST_CORPUS}
            --emit-manifest ${RP6502_TEST_CORPUS}/manifest.txt
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/roms.stamp
        DEPENDS ${RP6502_CORPUS_GEN} ${RP6502_TEST_LIB_FILES}
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

# rp6502_test_roms(<target> <file> <rom>...)
# Adds <target>, which runs the test file <file>, a path from tests/. The file
# writes each ROM <rom> and its .sym file into the ROM folder. The function
# sets <ROM>_ROM to the path of each ROM.
function(rp6502_test_roms target file)
    set(outputs)
    foreach(name IN LISTS ARGN)
        set(path ${RP6502_TEST_ROM_DIR}/${name}.rp6502)
        list(APPEND outputs ${path} ${RP6502_TEST_ROM_DIR}/${name}.sym)
        string(TOUPPER "${name}" var)
        string(MAKE_C_IDENTIFIER "${var}" var)
        set(${var}_ROM ${path} PARENT_SCOPE)
    endforeach()
    add_custom_command(OUTPUT ${outputs}
        COMMAND ${RP6502_TEST_PYTHON} ${RP6502_TESTS_DIR}/${file}
            --emit ${RP6502_TEST_ROM_DIR}
        DEPENDS ${RP6502_TESTS_DIR}/${file} ${RP6502_TEST_LIB_FILES}
        COMMENT "Assembling the ROMs of ${file}"
        VERBATIM)
    add_custom_target(${target} DEPENDS ${outputs})
    add_dependencies(rp6502_test_roms ${target})
endfunction()

rp6502_test_roms(aud_roms cpu/aud/aud.py
    psg psg_pre opl opl_exit opl_init bel bel_off opl_bel)
rp6502_test_roms(pcm_opl_roms cpu/aud/pcm_opl.py pcm+opl)
rp6502_test_roms(pcm_opl_or_psg_roms cpu/aud/pcm_opl_or_psg.py pcm+opl_or_psg)
rp6502_test_roms(dir_roms cpu/fs/dir.py dir)
rp6502_test_roms(drive_roms cpu/fs/drive.py drive)
rp6502_test_roms(execrel_roms cpu/fs/execrel.py execrel)
rp6502_test_roms(file_roms cpu/fs/file.py file)
rp6502_test_roms(fstest_roms cpu/fs/fstest.py fstest)
rp6502_test_roms(stream_roms cpu/fs/stream.py stream)
rp6502_test_roms(gamepad_roms cpu/hid/gamepad.py gamepad gamepad_bounds)
rp6502_test_roms(keyboard_roms cpu/hid/keyboard.py keyboard)
rp6502_test_roms(pointer_roms cpu/hid/pointer.py mouse tablet)
rp6502_test_roms(argv_roms cpu/ria/argv.py argv)
rp6502_test_roms(chain_roms cpu/ria/chain.py launcher child)
rp6502_test_roms(clk_roms cpu/ria/clk.py clk)
rp6502_test_roms(seed_roms cpu/ria/seed.py seed)
rp6502_test_roms(exec_roms cpu/ria/exec.py exec)
rp6502_test_roms(sigint_roms cpu/ria/sigint.py sigint)
rp6502_test_roms(stdio_roms cpu/ria/stdio.py stdio)
rp6502_test_roms(strftime_roms cpu/ria/strftime.py strftime)
rp6502_test_roms(tty_keys_roms cpu/ria/tty_keys.py tty_keys)
rp6502_test_roms(tty_pipe_roms cpu/ria/tty_pipe.py tty_pipe)
rp6502_test_roms(zeroread_roms cpu/ria/zeroread.py zeroread)
rp6502_test_roms(vsync_roms cpu/vga/vsync.py vsync)
rp6502_test_roms(bigfile_roms host/pocket/bigfile.py bigfile)
rp6502_test_roms(probe_roms host/pocket/probe.py probe)
rp6502_test_roms(sleepasset_roms host/pocket/sleepasset.py
    sleepasset-a sleepasset-b)
rp6502_test_roms(sleepfile_roms host/pocket/sleepfile.py sleepfile)

# rp6502_add_script_test(NAME <name> COMMAND <file> [<arg>...]
#                        [TIMEOUT <seconds>])
# Adds the test script.<name>. A .py <file> runs its test <name>, and any
# other <file> is an emulator script. Each <arg> goes to the emulator.
function(rp6502_add_script_test)
    cmake_parse_arguments(PARSE_ARGV 0 S "" "NAME;TIMEOUT" "COMMAND")
    if(NOT TARGET rp6502-emu)
        message(FATAL_ERROR
            "rp6502_add_script_test(NAME ${S_NAME}) drives the shipped binary.\n"
            "  Guard the call with if(TARGET rp6502-emu).")
    endif()
    if(NOT S_NAME OR NOT S_COMMAND OR S_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "rp6502_add_script_test(NAME <name> COMMAND <file> "
            "[<arg>...] [TIMEOUT <seconds>])")
    endif()
    set(args "${S_COMMAND}")
    list(POP_FRONT args file)
    if(NOT IS_ABSOLUTE ${file})
        set(file ${CMAKE_CURRENT_LIST_DIR}/${file})
    endif()
    # A relative path in a script's shot command or in a program's file calls
    # resolves against the working directory, so each script test gets its own
    # directory and tests run by ctest --parallel do not overwrite each other's
    # files. The same directory is the SAVE: folder, so no test writes into
    # the save folder of the user who runs it.
    set(_work ${CMAKE_CURRENT_BINARY_DIR}/script.${S_NAME})
    file(MAKE_DIRECTORY ${_work})

    file(RELATIVE_PATH _dir ${RP6502_TESTS_DIR} ${CMAKE_CURRENT_LIST_DIR})
    string(REPLACE "/" "." _dir "${_dir}")

    if(file MATCHES "\\.py$")
        add_test(NAME script.${S_NAME}
            COMMAND ${CMAKE_COMMAND} -E env RP6502_EMU=$<TARGET_FILE:rp6502-emu>
                RP6502_ROMS=${RP6502_TEST_ROM_DIR}
                PYTHONPATH=${RP6502_TEST_PYTHONPATH} PYTHONDONTWRITEBYTECODE=1
                python3 ${file} ${S_NAME} --save-dir ${_work} ${args})
    else()
        add_test(NAME script.${S_NAME}
            COMMAND rp6502-emu --save-dir ${_work} --script ${file} ${args})
    endif()
    if(NOT S_TIMEOUT)
        set(S_TIMEOUT 120)
    endif()
    set_tests_properties(script.${S_NAME} PROPERTIES
        WORKING_DIRECTORY ${_work}
        TIMEOUT ${S_TIMEOUT}
        LABELS "script;${_dir}")
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
