# What find_package(cc65) lands on:
#
#   set(CC65_TARGET_SYSTEM rp6502)
#   find_package(cc65 REQUIRED)
#
if(NOT CC65_TARGET_SYSTEM)
    message(FATAL_ERROR
        "Set CC65_TARGET_SYSTEM to a cc65 target before find_package(cc65).")
endif()
set(CC65_TARGET_SYSTEM "${CC65_TARGET_SYSTEM}" CACHE STRING "cc65 target system")
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cc65-toolchain.cmake")
