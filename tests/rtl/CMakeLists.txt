cmake_minimum_required(VERSION 3.21)

project(rp6502-rtl C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

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

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_path(SET RP6502_ROOT NORMALIZE ${CMAKE_CURRENT_LIST_DIR}/../..)
set(RP6502_SRC ${RP6502_ROOT}/src)
set(RP6502_VENDOR ${RP6502_ROOT}/vendor)

include(${RP6502_ROOT}/version.cmake)

find_package(verilator CONFIG REQUIRED
    HINTS $ENV{VERILATOR_ROOT} /usr/share/verilator)

include(${RP6502_SRC}/host/pocket/machine.cmake)
set(RP6502_SW_TTY ${CMAKE_CURRENT_LIST_DIR}/pocket/tty.c)
include(${RP6502_SRC}/host/pocket/sw.cmake)

if(WIN32)
    message(FATAL_ERROR
        "the verilated machine needs a POSIX host; "
        "the C suites are in src/host/sokol's tree")
elseif(APPLE)
    set(RP6502_OSAL ${RP6502_SRC}/osal/macos)
else()
    set(RP6502_OSAL ${RP6502_SRC}/osal/linux)
endif()
include(${RP6502_SRC}/core/emu.cmake)
include(${RP6502_SRC}/osal/posix/posix.cmake)
rp6502_osal_posix(emu_core TRANSPORT aio)
target_sources(emu_core PRIVATE ${RP6502_OSAL}/os.c)

target_include_directories(emu_core PUBLIC ${RP6502_ROOT}/src/host/sokol)

enable_testing()

add_test(NAME w65c02_gen
    COMMAND ${CMAKE_COMMAND} -E env python3 ${W65C02_GEN} --report)
set_tests_properties(w65c02_gen PROPERTIES LABELS "gate;rtl")
add_test(NAME rbf_r
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/core/gen/rbf_r_gen.py --check)
set_tests_properties(rbf_r PROPERTIES LABELS "gate;rtl")

include(${RP6502_ROOT}/tests/rp6502_tests.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rp6502_verilate.cmake)

# tests/cpu registers its simulator tests with the helpers that
# rp6502_verilate.cmake defines, so that file has to be included first.
add_subdirectory(${RP6502_ROOT}/tests/cpu cpu)

add_subdirectory(aud)
add_subdirectory(vga)
add_subdirectory(ria)
add_subdirectory(riscv)
add_subdirectory(wdc)
add_subdirectory(${RP6502_ROOT}/tests/host/pocket host/pocket)
