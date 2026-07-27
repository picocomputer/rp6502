# Shared CTest wiring. Each buildable target adds the test directory it owns
# (src/emu adds tests/emu, src/fpga adds tests/fpga); this file carries what
# both need. Uses utest.h (vendored with sokol).

set(RP6502_UTEST_DIR ${RP6502_VENDOR}/sokol/tests/functional)
set(RP6502_TEST_ROMS ${CMAKE_CURRENT_LIST_DIR}/roms)

# rp6502_add_test(<name> [SOURCES ...] [LIBS ...] [INCLUDES ...] [DEFS ...]
#                        [FIXTURE <file in roms/>] [TIMEOUT <seconds>])
#
# Builds test_<name> from test_<name>.c unless SOURCES says otherwise, and
# registers it as CTest <name>. FIXTURE becomes TEST_FIXTURE, the absolute path
# a test opens — every test uses at most one.
function(rp6502_add_test name)
    cmake_parse_arguments(T "" "FIXTURE;TIMEOUT" "SOURCES;LIBS;INCLUDES;DEFS" ${ARGN})

    if(NOT T_SOURCES)
        set(T_SOURCES test_${name}.c)
    endif()

    add_executable(test_${name} ${T_SOURCES})
    target_include_directories(test_${name} PRIVATE ${RP6502_UTEST_DIR} ${T_INCLUDES})

    if(T_LIBS)
        target_link_libraries(test_${name} PRIVATE ${T_LIBS})
    endif()
    if(T_FIXTURE)
        list(APPEND T_DEFS TEST_FIXTURE="${RP6502_TEST_ROMS}/${T_FIXTURE}")
    endif()
    if(T_DEFS)
        target_compile_definitions(test_${name} PRIVATE ${T_DEFS})
    endif()

    add_test(NAME ${name} COMMAND test_${name})
    if(T_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${T_TIMEOUT})
    endif()
endfunction()
