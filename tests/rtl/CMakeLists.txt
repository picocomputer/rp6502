# The verilated machine, and every claim that needs one.
#
# A project root rather than something a host build finds: elaborating the
# machine and compiling the five megabytes of C++ it becomes is not a cost
# anyone working on C should pay by accident, and Verilator being installed is
# not a reason to pay it. Opening this tree is how you ask.
#
# This root also adds tests/cpu — the machine's own suites, the same directory
# a desktop tree adds — so a claim that can be made of both machines is made
# here of this one, from the same source. What is in this directory is what
# has no other implementation to be held against.
#
# RP6502_SOFT_CPU  the RISC-V toolchain is present, so anything that needs a
#                  booted soft CPU can be registered.

cmake_minimum_required(VERSION 3.21)

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

cmake_path(SET RP6502_ROOT NORMALIZE ${CMAKE_CURRENT_LIST_DIR}/../..)
set(RP6502_SRC ${RP6502_ROOT}/src)
set(RP6502_VENDOR ${RP6502_ROOT}/vendor)

# First: it takes RP6502_CI out of the cache, so nothing may read it earlier.
include(${RP6502_ROOT}/version.cmake)

# REQUIRED, because this tree is nothing without it. The build that warned and
# carried on existed only while one tree served two masters.
find_package(verilator CONFIG REQUIRED
    HINTS $ENV{VERILATOR_ROOT} /usr/share/verilator)

# The machine as a list of files, the generated packages, and the soft CPU's
# firmware.
include(${RP6502_SRC}/core/machine.cmake)

# --- The emulator, for the suites that read it directly ---
# tests/cpu holds direct-C tests of the machine's own code — the filesystem,
# the code pages, the keyboard layouts — and this tree runs them beside the
# simulated ones. It is built here over this workstation's own host, which is
# the only part of it this tree has an opinion about.
if(WIN32)
    message(FATAL_ERROR
        "the verilated machine needs a POSIX host; "
        "the C suites are in src/mach/emu's tree")
elseif(APPLE)
    set(RP6502_HOST ${RP6502_SRC}/host/macos)
else()
    set(RP6502_HOST ${RP6502_SRC}/host/linux)
endif()
include(${RP6502_SRC}/core/emu.cmake)
include(${RP6502_SRC}/host/posix/posix.cmake)
target_sources(emu_core PRIVATE ${RP6502_HOST}/host.c)
set_property(TARGET emu_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})

enable_testing()

include(${RP6502_ROOT}/tests/rp6502_tests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rp6502_verilate.cmake)

# The machine's own suites, the same directory the desktop trees add. Its
# simulator-shaped registrations wake up here because rp6502_verilate.cmake
# above defined the helpers they guard on.
add_subdirectory(${RP6502_ROOT}/tests/cpu cpu)

# What only a simulator can answer, and the board this tree fits.
add_subdirectory(aud)
add_subdirectory(vid)
add_subdirectory(ria)
add_subdirectory(rv)
add_subdirectory(wdc)
add_subdirectory(${RP6502_ROOT}/tests/host/pocket host/pocket)
