cmake_minimum_required(VERSION 3.20)

project(rp6502-rtl C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(RP6502_RTL_SIM "Verilated machine and the test suite" ON)

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

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(RP6502_SRC ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
get_filename_component(RP6502_VENDOR ${RP6502_ROOT}/vendor ABSOLUTE)
set(RP6502_HOST_POCKET ${RP6502_SRC}/host/pocket/core)

include(${CMAKE_CURRENT_LIST_DIR}/machine.cmake)

if(RP6502_RTL_SIM)
    set(RP6502_EMU_CORE_ONLY ON CACHE BOOL "" FORCE)
    add_subdirectory(${RP6502_SRC}/emu emu)

    enable_testing()
    set(RP6502_TESTS_RTL ON)
    add_subdirectory(${RP6502_ROOT}/tests tests)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/quartus.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/synth.cmake)
include(${RP6502_SRC}/host/pocket/pocket.cmake)
