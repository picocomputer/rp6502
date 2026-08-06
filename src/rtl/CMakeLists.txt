cmake_minimum_required(VERSION 3.20)

project(rp6502-rtl C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# One source tree, four configurations, and the presets name them: the
# simulation twice over, the Pocket, and the synthesis probes. What varies is
# whether the verilated machine and its tests are part of it.
#
# Off is not a stripped-down build, it is a different job. A bitstream is
# Quartus and a RISC-V compiler and nothing else, and saying so is what lets
# the Pocket be built on a machine with no Verilator installed at all - which
# is what CI's bitstream runner is.
option(RP6502_RTL_SIM "Verilated machine and the test suite" ON)

# Ninja, the way CI configures this tree. Under Makefiles `cmake --build`
# takes one core unless told otherwise, and this tree is twenty-odd test
# binaries over a handful of shared verilated models — the shape that gains
# most from a build graph and least from one recipe at a time.
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
    # The oracle has to be the same machine the RTL is. This platform builds
    # term.c without the alternate screen buffer — the cells are block memory
    # here and that buffer is half of them — so the emu_core this tree builds
    # must match, or a frame comparison diverges the moment a script types
    # ?1049. This is a separate build tree from the emulator's, so its own
    # emu_core keeps the buffer.
    target_compile_definitions(emu_core PUBLIC TERM_ALT_SCREEN=0)

    enable_testing()
    # The whole suite, organised by subsystem rather than by host. This tree
    # has the RTL, so it registers the verilated half of every subsystem
    # alongside the C half — both implementations of the CPU against the same
    # vectors, both audio engines against the same reference samples, and so
    # on.
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
