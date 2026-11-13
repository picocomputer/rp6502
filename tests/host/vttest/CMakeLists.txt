# Thomas Dickey's vttest, run against the console terminal by vtrun. It needs
# a pty, so it is built only where there is one.
if(WIN32)
    return()
endif()

add_executable(vtrun vtrun.c ${RP6502_SRC}/host/sokol/cli/png.c
    ${RP6502_BENCH}/tb_seed.c ${RP6502_BENCH}/tb_log.c ${RP6502_SRC}/core/sys/crc32.c)
target_include_directories(vtrun PRIVATE ${RP6502_SRC} ${RP6502_BENCH})
target_link_libraries(vtrun PRIVATE emu_core)
if(NOT APPLE)
    target_link_libraries(vtrun PRIVATE util)
endif()

# vttest is a submodule, pinned so the screens it draws stay the same, and
# built with its own configure and make.
rp6502_submodule(vendor/vttest SENTINEL main.c
    WANTS "the vttest run" OPTIONAL RESULT RP6502_HAVE_VTTEST)
if(NOT RP6502_HAVE_VTTEST)
    return()
endif()

include(ExternalProject)
set(_vttest_bin ${CMAKE_CURRENT_BINARY_DIR}/vttest-build)
ExternalProject_Add(vttest_build
    SOURCE_DIR ${RP6502_VENDOR}/vttest
    BINARY_DIR ${_vttest_bin}
    CONFIGURE_COMMAND ${RP6502_VENDOR}/vttest/configure
    BUILD_COMMAND make
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${_vttest_bin}/vttest)
add_dependencies(vtrun vttest_build)

# The walk takes about four minutes, most of it vttest's own pauses. With the
# approved screens in golden/, every screen is compared with its golden copy.
set(_golden ${CMAKE_CURRENT_LIST_DIR}/golden)
set(_env VTTEST=${_vttest_bin}/vttest)
if(EXISTS ${_golden})
    list(APPEND _env VTGOLDEN=${_golden})
endif()
add_test(NAME vttest
    COMMAND ${CMAKE_COMMAND} -E env ${_env}
        $<TARGET_FILE:vtrun> ${CMAKE_CURRENT_BINARY_DIR}/screens)
set_tests_properties(vttest PROPERTIES TIMEOUT 600 LABELS "slow;host.vttest"
    FIXTURES_REQUIRED vttest_screens)
add_test(NAME vttest_screens_dir
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/screens)
set_tests_properties(vttest_screens_dir PROPERTIES FIXTURES_SETUP vttest_screens)
