cmake_minimum_required(VERSION 3.20)

project(rp6502-fpga C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Emit compile_commands.json

get_filename_component(RP6502_SRC ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
get_filename_component(RP6502_VENDOR ${RP6502_ROOT}/vendor ABSOLUTE)
set(RP6502_FPGA_RTL ${CMAKE_CURRENT_LIST_DIR}/rtl)
set(RP6502_FPGA_POCKET ${CMAKE_CURRENT_LIST_DIR}/platform/pocket/core)

# emu_core is the reference oracle. Core-only: no window, debugger or its own
# tests, so the simulation needs none of the desktop dev stack.
set(RP6502_EMU_CORE_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(${RP6502_SRC}/emu emu)
enable_testing()
# The whole suite, organised by subsystem rather than by host. This tree has
# the RTL, so it registers the verilated half of every subsystem alongside the
# C half — both implementations of the CPU against the same vectors, both
# audio engines against the same reference samples, and so on.
set(RP6502_TESTS_RTL ON)
add_subdirectory(${RP6502_ROOT}/tests tests)
