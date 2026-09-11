# The emulator as a libretro core, for RetroArch and the launchers that embed
# it. A frontend owns the window, the audio device, the input and the loop, so
# this root takes emu_core and none of sokol.cmake.

cmake_minimum_required(VERSION 3.21)

project(rp6502-emu-libretro C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_path(SET RP6502_ROOT NORMALIZE ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(RP6502_SRC ${RP6502_ROOT}/src)
set(RP6502_VENDOR ${RP6502_ROOT}/vendor)

# First: version.cmake takes RP6502_CI out of the cache, so nothing may read
# it earlier.
include(${RP6502_ROOT}/version.cmake)

# Copied, not generated. A frontend prints the version retro_get_system_info
# returns, and reads display_version out of this file only for a core that is
# not running, so the file carries no version and this copy is what libretro's
# own dist/info holds.
configure_file(${CMAKE_CURRENT_LIST_DIR}/dist/rp6502_libretro.info
    ${CMAKE_BINARY_DIR}/rp6502_libretro.info COPYONLY)

# The OS this build is for. ANDROID is tested before the final else because
# the NDK toolchain also sets UNIX.
if(WIN32)
    set(RP6502_OSAL ${RP6502_SRC}/osal/windows)
elseif(ANDROID)
    set(RP6502_OSAL ${RP6502_SRC}/osal/android)
elseif(APPLE)
    set(RP6502_OSAL ${RP6502_SRC}/osal/macos)
else()
    set(RP6502_OSAL ${RP6502_SRC}/osal/linux)
endif()
# A Release build says nothing at all by default, which leaves a failed load
# with no explanation at any frontend verbosity. ERROR costs a few strings.
set(RP6502_LOG_LEVEL ERROR CACHE STRING
    "What every log category says: NONE, ERROR, WARN, INFO, DEBUG")

include(${RP6502_SRC}/core/emu.cmake)
# This machine's own directory, because core/sys/sys.c includes drivers.h by
# bare name and emu.cmake puts no machine directory on the path.
target_include_directories(emu_core PUBLIC ${CMAKE_CURRENT_LIST_DIR})

rp6502_submodule(vendor/libretro-common SENTINEL include/libretro.h
    SPARSE include WANTS "the libretro API header")
set(RP6502_LIBRETRO_INCLUDE ${RP6502_VENDOR}/libretro-common/include)

# Both arms take the synchronous transport, so a savestate never has to cancel
# a read that is still in flight. On POSIX it also means a frontend unloading
# this shared library leaves no glibc AIO helper thread holding a buffer inside
# it. Windows uses a handle flag rather than helper threads either way.
if(WIN32)
    include(${RP6502_SRC}/osal/windows/windows.cmake)
    rp6502_osal_windows(emu_core TRANSPORT sync)
else()
    include(${RP6502_SRC}/osal/posix/posix.cmake)
    rp6502_osal_posix(emu_core TRANSPORT sync)
    target_sources(emu_core PRIVATE ${RP6502_OSAL}/os.c)
endif()

# Objects linked into a shared library have to be position-independent, and
# every object emu_core carries is linked into this one.
set_property(TARGET emu_core PROPERTY POSITION_INDEPENDENT_CODE ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)


# The libretro buildbot builds ${CORENAME}_libretro and a frontend loads
# rp6502_libretro.so, so the target name has to be the file name and PREFIX
# is what keeps the "lib" off.
add_library(rp6502_libretro SHARED
    ${CMAKE_CURRENT_LIST_DIR}/retro.c
    ${CMAKE_CURRENT_LIST_DIR}/input.c
    ${RP6502_SRC}/core/sys/crc32.c
    ${RP6502_SRC}/core/sys/version.c)
set_target_properties(rp6502_libretro PROPERTIES PREFIX "")
target_link_libraries(rp6502_libretro PRIVATE emu_core)
target_include_directories(rp6502_libretro PRIVATE ${RP6502_LIBRETRO_INCLUDE})
target_include_directories(emu_core PUBLIC ${RP6502_LIBRETRO_INCLUDE})

# The libretro ABI is the entire export surface. libretro.h marks the retro_*
# definitions visible itself, so hiding everything else by default leaves
# exactly those; the ELF and Mach-O arms below then say so a second time,
# because -fvisibility=hidden reaches only what this build compiles and a
# static archive linked in, libgcc among them, arrives with its own symbols
# visible. MSVC needs none of this, because dllexport is the whole mechanism
# there.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(emu_core PRIVATE -fvisibility=hidden)
    target_compile_options(rp6502_libretro PRIVATE -fvisibility=hidden)
    if(APPLE)
        # ld64 takes an export list, not a version script.
        target_link_options(rp6502_libretro PRIVATE
            -Wl,-exported_symbols_list,${CMAKE_CURRENT_LIST_DIR}/link_apple.exp)
        set_target_properties(rp6502_libretro PROPERTIES
            LINK_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/link_apple.exp)
    elseif(NOT WIN32)
        target_link_options(rp6502_libretro PRIVATE
            -Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/link.T
            -Wl,--no-undefined)
        set_target_properties(rp6502_libretro PROPERTIES
            LINK_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/link.T)
    endif()
endif()

rp6502_use_version_header(rp6502_libretro ${RP6502_SRC}/core/sys/version.c emu_core)

# src/host/sokol builds its tests unconditionally because it is only ever
# somebody's working tree. This root is also configured by the libretro
# buildbot, which has no python3 and no use for the generated ROM corpus.
option(RP6502_TESTS "Build the test suite" ON)
if(RP6502_TESTS)
    enable_testing()
    include(${RP6502_ROOT}/tests/rp6502_tests.cmake)
    add_subdirectory(${RP6502_ROOT}/tests/host/libretro tests/host/libretro)
    add_subdirectory(${RP6502_ROOT}/tests/cpu tests/cpu)
endif()
