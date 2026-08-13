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

# The machine as a list of files, the generated packages, and the soft CPU's
# firmware. Every configuration needs these; none of them needs a simulator.
include(${CMAKE_CURRENT_LIST_DIR}/machine.cmake)

if(RP6502_RTL_SIM)
    # emu_core is the reference oracle. Core-only: no window, debugger or its
    # own tests, so the simulation needs none of the desktop dev stack.
    set(RP6502_EMU_CORE_ONLY ON CACHE BOOL "" FORCE)
    add_subdirectory(${RP6502_SRC}/emu emu)

    enable_testing()
    # The whole suite, organised by subsystem rather than by host: this tree
    # has the RTL, so it registers the verilated half of every subsystem
    # alongside the C half, against the same vectors.
    set(RP6502_TESTS_RTL ON)
    add_subdirectory(${RP6502_ROOT}/tests tests)
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
