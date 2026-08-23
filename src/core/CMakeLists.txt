cmake_minimum_required(VERSION 3.20)

project(rp6502-rtl C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# One source tree, four configurations, and the presets name them: the
# simulation twice over, the Pocket, and the synthesis probes. What varies is
# whether the verilated machine and its tests are part of it.
#
# Off is not a stripped-down build, it is a different job: a bitstream is
# Quartus and a RISC-V compiler and nothing else, which is what lets the
# Pocket be built on CI's bitstream runner with no Verilator installed.
option(RP6502_RTL_SIM "Verilated machine and the test suite" ON)

# Ninja, the way CI configures this tree. Under Makefiles `cmake --build`
# takes one core unless told otherwise.
if(RP6502_RTL_SIM AND NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR
        "This tree wants Ninja; ${CMAKE_GENERATOR} builds single-core.\n"
        "  rm -rf ${CMAKE_BINARY_DIR}\n"
        "  cmake -S ${CMAKE_CURRENT_LIST_DIR} -B ${CMAKE_BINARY_DIR} "
        "-G Ninja -DCMAKE_BUILD_TYPE=Release")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Emit compile_commands.json

get_filename_component(RP6502_SRC ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
get_filename_component(RP6502_VENDOR ${RP6502_ROOT}/vendor ABSOLUTE)
set(RP6502_HOST_POCKET ${RP6502_SRC}/host/pocket/core)

# rp6502_inputs(<name> <file>...)
#
# Writes inputs/<name>, one repository-relative path per line, for a build that
# is not this one. CI decides whether a job is owed before it has a tree to ask,
# and the only thing that knows what a job reads is the code that reads it. A
# workflow keeping its own list of directories is a second copy of these
# DEPENDS, and was one: a glob woke the fit for the Pocket's firmware, which
# costs an assemble, and never woke it for the two Pico firmwares, of which it
# compiles about twenty of a hundred and thirty.
#
# Relative and sorted so the answer does not depend on where the tree sits or
# what order a list was built in. Anything inside the build directory is
# dropped: a generated file cannot appear in a diff, and what actually decides
# one is its generator, which is named instead.
function(rp6502_inputs name)
    set(_rel "")
    foreach(_f IN LISTS ARGN)
        string(FIND ${_f} ${CMAKE_BINARY_DIR} _in_build)
        if(NOT _in_build EQUAL 0)
            file(RELATIVE_PATH _r ${RP6502_ROOT} ${_f})
            list(APPEND _rel ${_r})
        endif()
    endforeach()
    list(SORT _rel)
    list(REMOVE_DUPLICATES _rel)
    string(REPLACE ";" "\n" _text "${_rel}")
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/inputs/${name} CONTENT "${_text}\n")
endfunction()

# Before anything that reads it: src/core/emu includes this too, and the one that
# runs first is the one that takes RP6502_CI out of the cache.
include(${RP6502_ROOT}/version.cmake)

# The machine as a list of files, the generated packages, and the soft CPU's
# firmware. Every configuration needs these; none of them needs a simulator.
include(${CMAKE_CURRENT_LIST_DIR}/machine.cmake)

# Which submodules the machine itself wanted, taken here because the tests
# below ask for their own and a bitstream is not made from those. The gate
# configures this tree with the simulator on, so without a mark at this line
# there would be no telling the two apart.
get_property(RP6502_MACHINE_MODULES GLOBAL PROPERTY RP6502_SUBMODULES)

if(RP6502_RTL_SIM)
    # emu_core is the reference oracle. Core-only: no window, debugger or its
    # own tests, so the simulation needs none of the desktop dev stack.
    set(RP6502_EMU_CORE_ONLY ON CACHE BOOL "" FORCE)
    add_subdirectory(${RP6502_SRC}/core/emu emu)

    enable_testing()
    # The whole suite, organised by subsystem rather than by host: this tree
    # has the RTL, so it registers the verilated half of every subsystem
    # alongside the C half, against the same vectors.
    set(RP6502_TESTS_RTL ON)
    add_subdirectory(${RP6502_ROOT}/tests tests)

    # Taken here for the same reason the machine's were, and from the other
    # side: below this line the only thing still asking is the Pocket, and
    # Analogue's framework is nothing a simulation reads.
    get_property(RP6502_SIM_MODULES GLOBAL PROPERTY RP6502_SUBMODULES)
endif()

# Synthesis. Each of these registers nothing at all without Quartus, so a
# machine with only a simulator still configures every one of them.
#
# The hosts are the one thing this tree reaches sideways for. src/host is not
# ours — the emulator's backends live there too — so a host is included by
# absolute path rather than owned, and adding MiSTer is one more line here.
include(${CMAKE_CURRENT_LIST_DIR}/quartus.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/synth.cmake)
include(${RP6502_SRC}/host/pocket/pocket.cmake)

# What the simulation is made from, last because the subdirectories above are
# what fill it. Every part of it is something that already knew: the machine
# and its firmware are lists here, the oracle is emu_core's own, every test
# arrives through rp6502_add_test and every generated asset through
# rp6502_machine_asset. Only the benches are swept, because they are headers
# on an include path and no target lists those — the way machine.cmake sweeps
# the firmware's for its own DEPENDS.
if(RP6502_RTL_SIM)
    get_property(TEST_INPUTS GLOBAL PROPERTY RP6502_TEST_INPUTS)
    get_property(ASSET_INPUTS GLOBAL PROPERTY RP6502_ASSET_INPUTS)
    get_target_property(ORACLE_INPUTS emu_core SOURCES)
    file(GLOB BENCH_INPUTS ${RP6502_ROOT}/tests/bench/*)
    rp6502_inputs(sim-inputs.txt
        ${RP6502_MACHINE_SOURCES} ${SW_SOURCES} ${SW_HEADERS} ${SW_SRC}/link.ld
        ${TEST_INPUTS} ${ASSET_INPUTS} ${ORACLE_INPUTS} ${BENCH_INPUTS}
        # hazard3 is reached off verilator's -y and sokol carries the harness,
        # so neither names a file any target lists; a bump of any of them
        # reaches a diff only as its gitlink.
        ${RP6502_SIM_MODULES}
        ${CMAKE_CURRENT_LIST_FILE}
        ${CMAKE_CURRENT_LIST_DIR}/machine.cmake
        ${CMAKE_CURRENT_LIST_DIR}/assets.cmake
        ${RP6502_SRC}/core/emu/CMakeLists.txt
        ${RP6502_ROOT}/tests/CMakeLists.txt
        ${RP6502_ROOT}/tests/rp6502_tests.cmake
        ${RP6502_ROOT}/tests/rp6502_verilate.cmake)
endif()
