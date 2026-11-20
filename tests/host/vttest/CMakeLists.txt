# Thomas Dickey's vttest, run against the console terminal by vtrun. It is
# built only on Linux: the walk reads /proc to see vttest wait for a key.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

add_executable(vtrun vtrun.c xmirror.c ${RP6502_SRC}/host/sokol/cli/png.c
    ${RP6502_BENCH}/tb_seed.c ${RP6502_BENCH}/tb_log.c ${RP6502_BENCH}/tb_version.c
    ${RP6502_SRC}/core/sys/crc32.c)
target_include_directories(vtrun PRIVATE ${RP6502_SRC} ${RP6502_BENCH})
target_link_libraries(vtrun PRIVATE emu_core util)

# vttest's pauses, preloaded into it so they end as soon as vtrun has
# answered what came before them.
add_library(vtsleep SHARED vtsleep.c)
target_link_libraries(vtsleep PRIVATE dl)

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
add_dependencies(vtrun vttest_build vtsleep)

# The walk takes about 25 seconds: vtsleep ends vttest's pauses; vtrun
# gives up after eight minutes. golden.txt holds the approved screens: a CRC
# of the cells of each and its cursor. A screen that differs, is new or is
# gone fails the test and is saved in screens/ as text and PNG. To approve a
# changed walk, look at those, then copy screens/screens.txt to golden.txt.
set(_golden ${CMAKE_CURRENT_LIST_DIR}/golden.txt)
set(_screens ${CMAKE_CURRENT_BINARY_DIR}/screens)
set(_env VTTEST=${_vttest_bin}/vttest VTSLEEP=$<TARGET_FILE:vtsleep>)
if(EXISTS ${_golden})
    list(APPEND _env VTGOLDEN=${_golden})
endif()

# Where xterm and Xvfb are installed, the walk also gives everything to a real
# xterm and holds each screen against xterm's. xterm.txt lists the screens
# that differ on purpose, each with a digest of how; any other difference, or
# one of those that has changed or gone, fails the test and is shown in
# screens/xterm/. To approve them, look at those, then copy the lines you agree
# with from screens/xterm.txt, with why, into xterm.txt.
#
# The xterm is patch 412 or later, the one xterm.txt was made against; an
# older one draws a few screens differently. Where the system's is older,
# build xterm from https://invisible-island.net/xterm/ and name it with
# -DRP6502_XTERM=/path/to/xterm.
find_program(RP6502_XTERM xterm)
find_program(RP6502_XVFB Xvfb)
set(_xknown ${CMAKE_CURRENT_LIST_DIR}/xterm.txt)
set(_xterm_ok FALSE)
if(RP6502_XTERM AND RP6502_XVFB AND EXISTS ${_xknown})
    execute_process(COMMAND ${RP6502_XTERM} -version OUTPUT_VARIABLE _xv
        ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_xv MATCHES "XTerm\\(([0-9]+)\\)" AND CMAKE_MATCH_1 GREATER_EQUAL 412)
        set(_xterm_ok TRUE)
        list(APPEND _env VTXTERM=${RP6502_XTERM} VTXKNOWN=${_xknown})
    else()
        message(STATUS "vttest: ${RP6502_XTERM} is older than patch 412; "
            "not comparing with xterm")
    endif()
endif()
add_test(NAME vttest
    COMMAND ${CMAKE_COMMAND} -E env ${_env} $<TARGET_FILE:vtrun> ${_screens})
set_tests_properties(vttest PROPERTIES TIMEOUT 600 LABELS "slow;host.vttest"
    FIXTURES_REQUIRED vttest_screens)
# The last run's screens go first, so what is there after a run is its own.
add_test(NAME vttest_screens_clean COMMAND ${CMAKE_COMMAND} -E rm -rf ${_screens})
set_tests_properties(vttest_screens_clean PROPERTIES FIXTURES_SETUP vttest_screens)

# tmux, the program most likely to use the margins and rectangles now that
# the console says it is a VT420, run in panes through the console and held
# against xterm, where both are installed. Its locale is C, as the console
# has no UTF-8: in a UTF-8 locale tmux draws its borders in UTF-8.
find_program(RP6502_TMUX tmux)
if(RP6502_TMUX AND _xterm_ok)
    set(_tmux_screens ${CMAKE_CURRENT_BINARY_DIR}/tmux)
    add_test(NAME vttest_tmux
        COMMAND ${CMAKE_COMMAND} -E env VTXTERM=${RP6502_XTERM}
            "VTCMD=LC_ALL=C LANG=C exec ${RP6502_TMUX} -L rp6502-vtrun-$$ -f /dev/null new-session -x 80 -y 30 sh \\; split-window -h sh \\; split-window -v sh"
            "VTKEYS=seq space 1 space 50 return|ctrl-b o|seq space 100 space 160 return|ctrl-b o|ls space -l space / return|clear return|ctrl-b z|ctrl-b z|exit return|exit return|exit return"
            $<TARGET_FILE:vtrun> ${_tmux_screens})
    set_tests_properties(vttest_tmux PROPERTIES TIMEOUT 120 LABELS "slow;host.vttest")
endif()

