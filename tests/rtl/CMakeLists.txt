# The verilated machine, and every claim that needs one.
#
# A project root rather than something a host build finds: elaborating the
# machine and compiling the five megabytes of C++ it becomes is not a cost
# anyone working on C should pay by accident, and Verilator being installed is
# not a reason to pay it. Opening this tree is how you ask.
#
# The other half is tests/emu, which the desktop hosts add to their own build.
# Where a claim is about both machines it is made from both trees against one
# set of evidence: the corpora, the DUT interface and the shared drivers live
# beside the emulator's implementation and are named here by path.
#
# RP6502_SOFT_CPU  the RISC-V toolchain is present, so anything that needs a
#                  booted soft CPU can be registered.

cmake_minimum_required(VERSION 3.20)

project(rp6502-rtl C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Ninja, the way this tree is configured everywhere. Under Makefiles
# `cmake --build` takes one core unless told otherwise, and this is two dozen
# test binaries over a handful of shared verilated models.
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
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

get_filename_component(RP6502_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src ABSOLUTE)
get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
get_filename_component(RP6502_VENDOR ${RP6502_ROOT}/vendor ABSOLUTE)

# First: it takes RP6502_CI out of the cache, so nothing may read it earlier.
include(${RP6502_ROOT}/version.cmake)

# REQUIRED, because this tree is nothing without it. The build that warned and
# carried on existed only while one tree served two masters.
find_package(verilator CONFIG REQUIRED
    HINTS $ENV{VERILATOR_ROOT} /usr/share/verilator)

# The machine as a list of files, the generated packages, and the soft CPU's
# firmware.
include(${RP6502_SRC}/core/machine.cmake)

# --- The reference oracle ---
# Every comparison runs the same program on emu_core and on the verilated
# machine, so emu_core defines the behavior the RTL has to reproduce. It is
# built here over this workstation's own host, which is the only part of it
# this tree has an opinion about.
if(WIN32)
    message(FATAL_ERROR
        "the verilated machine needs a POSIX host for its oracle; "
        "the C suites are in src/host/win's tree")
elseif(APPLE)
    set(RP6502_HOST ${RP6502_SRC}/host/macos)
else()
    set(RP6502_HOST ${RP6502_SRC}/host/linux)
endif()
include(${RP6502_SRC}/core/emu/emu.cmake)
include(${RP6502_SRC}/core/posix/posix.cmake)
target_sources(emu_core PRIVATE ${RP6502_HOST}/host.c)
set_property(TARGET emu_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})

enable_testing()

include(${RP6502_ROOT}/tests/rp6502_tests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rp6502_verilate.cmake)

add_subdirectory(${RP6502_ROOT}/tests/bench bench)

add_subdirectory(aud)
add_subdirectory(vid)
add_subdirectory(mem)
add_subdirectory(ria)
add_subdirectory(rv)
add_subdirectory(wdc)
add_subdirectory(pocket)

# What the simulation is made from, last because the subdirectories above are
# what fill it. Every part of it is something that already knew: the machine
# and its firmware are lists here, the oracle is emu_core's own, every test
# arrives through rp6502_add_test and every generated asset through
# rp6502_machine_asset. Only the benches are swept, because they are headers
# on an include path and no target lists those — the way machine.cmake sweeps
# the firmware's for its own DEPENDS.
#
# The emulator's suite is in the list too. It is not built here, but a change
# to a corpus or a DUT this tree compiles from tests/emu is a change to the
# simulation, and the file that decides it has to be named where it lives.
get_property(TEST_INPUTS GLOBAL PROPERTY RP6502_TEST_INPUTS)
get_property(ASSET_INPUTS GLOBAL PROPERTY RP6502_ASSET_INPUTS)
get_property(RP6502_SUBMODULES GLOBAL PROPERTY RP6502_SUBMODULES)
get_target_property(ORACLE_INPUTS emu_core SOURCES)
file(GLOB BENCH_INPUTS ${RP6502_ROOT}/tests/bench/*)
rp6502_inputs(sim-inputs.txt
    ${RP6502_MACHINE_SOURCES} ${SW_SOURCES} ${SW_HEADERS} ${SW_SRC}/link.ld
    ${TEST_INPUTS} ${ASSET_INPUTS} ${ORACLE_INPUTS} ${BENCH_INPUTS}
    # hazard3 is reached off verilator's -y and sokol carries the harness,
    # so neither names a file any target lists; a bump of any of them
    # reaches a diff only as its gitlink.
    ${RP6502_SUBMODULES}
    ${CMAKE_CURRENT_LIST_FILE}
    ${CMAKE_CURRENT_LIST_DIR}/rp6502_verilate.cmake
    ${RP6502_SRC}/core/machine.cmake
    ${RP6502_SRC}/core/assets.cmake
    ${RP6502_SRC}/core/emu/emu.cmake
    ${RP6502_SRC}/core/posix/posix.cmake
    ${RP6502_ROOT}/tests/rp6502_tests.cmake)
