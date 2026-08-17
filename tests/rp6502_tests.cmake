# Shared CTest wiring. Both trees add tests/ and get the suites their build
# can run; this file carries what all of them need. Uses utest.h (vendored
# with sokol).

# Entered from both trees, so it asks rather than assuming the emulator's
# side already did.
include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/sokol SENTINEL tests/functional/utest.h
    WANTS "the test harness")

set(RP6502_UTEST_DIR ${RP6502_VENDOR}/sokol/tests/functional)
set(RP6502_TESTS_DIR ${CMAKE_CURRENT_LIST_DIR})
set(RP6502_TEST_ROMS ${RP6502_TESTS_DIR}/roms)

# The harness both sides share: emu_boot.h and the sys shims for the C tests,
# the tb_* benches and the oracle for the RTL ones. Every test gets it on the
# include path, because which of them a test needs is not worth stating.
set(RP6502_BENCH ${RP6502_TESTS_DIR}/bench)

# The video-mode corpus is generated, not committed. Every byte of it comes
# out of vidmodes.py, so a committed copy is only a second copy that can
# disagree with its generator. What stays in roms/ is the cc65-built programs,
# which have no generator and are reached by FIXTURE rather than from here.
set(RP6502_TEST_CORPUS ${CMAKE_BINARY_DIR}/roms)
if(NOT TARGET rp6502_test_corpus)
    set(RP6502_CORPUS_GEN ${RP6502_TEST_ROMS}/vidmodes.py)
    # It assembles through src/gen/rp6502_rom.py like every other ROM
    # generator, so a change there is a change to the corpus.
    set(RP6502_CORPUS_ASM ${RP6502_ROOT}/src/gen/rp6502_rom.py)
    # A stamp rather than the forty-one names: listing them here would be the
    # same duplication in a different file.
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/roms.stamp
        COMMAND ${CMAKE_COMMAND} -E env python3
            ${RP6502_CORPUS_GEN} --out ${RP6502_TEST_CORPUS}
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/roms.stamp
        DEPENDS ${RP6502_CORPUS_GEN} ${RP6502_CORPUS_ASM}
        COMMENT "Generating the video-mode ROM corpus"
        VERBATIM)
    add_custom_target(rp6502_test_corpus DEPENDS ${CMAKE_BINARY_DIR}/roms.stamp)
endif()

# rp6502_add_test(<name> [SOURCES ...] [LIBS ...] [INCLUDES ...] [DEFS ...]
#                        [FIXTURE <file in roms/>] [TIMEOUT <seconds>] [SPLIT])
#
# Builds test_<name> from test_<name>.c unless SOURCES says otherwise, and
# registers it as CTest <name>. FIXTURE becomes TEST_FIXTURE, the absolute path
# a test opens — every test uses at most one. TEST_SCRATCH is where a test
# writes throwaway files, so a run from any directory never litters the tree.
#
# SPLIT registers each UTEST case as its own CTest test instead. For the long
# ones that is the difference between a suite bounded by its slowest binary and
# one bounded by its slowest case; for the forty that finish in a fifth of a
# second it would cost more in process starts than it saves, which is why it is
# asked for rather than assumed.
function(rp6502_add_test name)
    cmake_parse_arguments(T "SPLIT" "FIXTURE;TIMEOUT" "SOURCES;LIBS;INCLUDES;DEFS" ${ARGN})

    if(NOT T_SOURCES)
        set(T_SOURCES test_${name}.c)
    endif()

    # Every test in both trees comes through here, so this is the one place
    # that can say what the suite is made of without naming any of it.
    foreach(_s IN LISTS T_SOURCES)
        if(NOT IS_ABSOLUTE ${_s})
            set(_s ${CMAKE_CURRENT_LIST_DIR}/${_s})
        endif()
        set_property(GLOBAL APPEND PROPERTY RP6502_TEST_INPUTS ${_s})
    endforeach()

    add_executable(test_${name} ${T_SOURCES})
    target_include_directories(test_${name} PRIVATE
        ${RP6502_UTEST_DIR} ${RP6502_BENCH} ${T_INCLUDES})

    if(T_LIBS)
        target_link_libraries(test_${name} PRIVATE ${T_LIBS})
    endif()
    list(APPEND T_DEFS TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}")
    # A bare name is one of the committed programs in roms/; a name with a
    # slash is relative to tests/, for fixtures that are not .rp6502 at all
    # and belong beside the suite that reads them.
    if(T_FIXTURE MATCHES "/")
        list(APPEND T_DEFS TEST_FIXTURE="${RP6502_TESTS_DIR}/${T_FIXTURE}")
    elseif(T_FIXTURE)
        list(APPEND T_DEFS TEST_FIXTURE="${RP6502_TEST_ROMS}/${T_FIXTURE}")
    endif()
    if(T_DEFS)
        target_compile_definitions(test_${name} PRIVATE ${T_DEFS})
    endif()

    add_dependencies(test_${name} rp6502_test_corpus)

    set(_cases ${name})
    if(T_SPLIT)
        # utest.h takes --filter but cannot list what it holds, so the cases are
        # read out of the source. Every one of them is a plain one-line UTEST(),
        # and the file is a configure dependency, so adding a case is enough to
        # register it.
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
        if(T_TIMEOUT)
            set_tests_properties(${_case} PROPERTIES TIMEOUT ${T_TIMEOUT})
        endif()
    endforeach()
    if(T_SPLIT)
        # A filter that matches nothing runs nothing and exits zero, which is a
        # green test that never ran.
        set_tests_properties(${_cases} PROPERTIES
            FAIL_REGULAR_EXPRESSION "Running 0 test cases")
    endif()
endfunction()
