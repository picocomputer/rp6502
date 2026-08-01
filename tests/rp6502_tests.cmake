# Shared CTest wiring. Both trees add tests/ and get the suites their build
# can run; this file carries what all of them need. Uses utest.h (vendored
# with sokol).

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
    # A stamp rather than the forty-one names: listing them here would be the
    # same duplication in a different file.
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/roms.stamp
        COMMAND ${CMAKE_COMMAND} -E env python3
            ${RP6502_CORPUS_GEN} --out ${RP6502_TEST_CORPUS}
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/roms.stamp
        DEPENDS ${RP6502_CORPUS_GEN}
        COMMENT "Generating the video-mode ROM corpus"
        VERBATIM)
    add_custom_target(rp6502_test_corpus DEPENDS ${CMAKE_BINARY_DIR}/roms.stamp)
endif()

# rp6502_add_test(<name> [SOURCES ...] [LIBS ...] [INCLUDES ...] [DEFS ...]
#                        [FIXTURE <file in roms/>] [TIMEOUT <seconds>])
#
# Builds test_<name> from test_<name>.c unless SOURCES says otherwise, and
# registers it as CTest <name>. FIXTURE becomes TEST_FIXTURE, the absolute path
# a test opens — every test uses at most one. TEST_SCRATCH is where a test
# writes throwaway files, so a run from any directory never litters the tree.
function(rp6502_add_test name)
    cmake_parse_arguments(T "" "FIXTURE;TIMEOUT" "SOURCES;LIBS;INCLUDES;DEFS" ${ARGN})

    if(NOT T_SOURCES)
        set(T_SOURCES test_${name}.c)
    endif()

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

    add_test(NAME ${name} COMMAND test_${name})
    if(T_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${T_TIMEOUT})
    endif()
endfunction()
