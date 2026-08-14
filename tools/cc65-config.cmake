# What find_package(cc65) lands on. All it does is name the toolchain file,
# the same shape llvm-mos-sdk-config.cmake has, so the two compilers are
# selected the same way:
#
#   set(CC65_TARGET_SYSTEM rp6502)
#   find_package(cc65 REQUIRED)
#
# Cached because a toolchain file is re-read in scopes that inherit nothing
# but the cache.

if(NOT CC65_TARGET_SYSTEM)
    message(FATAL_ERROR
        "Set CC65_TARGET_SYSTEM to a cc65 target before find_package(cc65).")
endif()

set(CC65_TARGET_SYSTEM "${CC65_TARGET_SYSTEM}" CACHE STRING "cc65 target system")
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cc65.cmake")
