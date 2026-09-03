# The emulator as a libretro core, for RetroArch and the launchers that
# embed it.
#
# The thinnest host there is: no window, no audio device, no input loop, no
# main(). A frontend owns all four and calls retro_run once a frame. So this
# root takes emu_core and none of sokol.cmake — the sokol window, the command
# line, the script channel and the debugger are all things a core has no use
# for.

cmake_minimum_required(VERSION 3.21)

project(rp6502-emu-libretro C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Emit compile_commands.json

cmake_path(SET RP6502_ROOT NORMALIZE ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(RP6502_SRC ${RP6502_ROOT}/src)
set(RP6502_VENDOR ${RP6502_ROOT}/vendor)

# First: it takes RP6502_CI out of the cache, so nothing may read it earlier.
include(${RP6502_ROOT}/version.cmake)

# The core's metadata carries a version, and this repository has exactly one
# of those. Strip the word a frontend prints for itself, which is the same
# strip retro_get_system_info does and the same one the Pocket does for
# core.json — so the .info in a release zip says what the binary beside it
# reports, tag or CI run or the day someone built it.
set(RP6502_INFO_VERSION "${RP6502_VERSION_BARE}")
configure_file(${CMAKE_CURRENT_LIST_DIR}/dist/rp6502_libretro.info.in
    ${CMAKE_BINARY_DIR}/rp6502_libretro.info @ONLY)

# The OS this build is for. One of the seven jobs in .gitlab-ci.yml lands in
# each arm; ANDROID is checked first because the NDK toolchain sets UNIX too.
if(WIN32)
    set(RP6502_OSAL ${RP6502_SRC}/osal/windows)
elseif(ANDROID)
    set(RP6502_OSAL ${RP6502_SRC}/osal/android)
elseif(APPLE)
    set(RP6502_OSAL ${RP6502_SRC}/osal/macos)
else()
    set(RP6502_OSAL ${RP6502_SRC}/osal/linux)
endif()
include(${RP6502_SRC}/core/emu.cmake)
# This machine's own directory, for the drivers.h core/sys/sys.c includes by
# bare name. emu.cmake puts src/ on the path and nothing more specific.
target_include_directories(emu_core PUBLIC ${CMAKE_CURRENT_LIST_DIR})

rp6502_submodule(vendor/libretro-common SENTINEL include/libretro.h
    SPARSE include WANTS "the libretro API header")
set(RP6502_LIBRETRO_INCLUDE ${RP6502_VENDOR}/libretro-common/include)

# The host OS seam. Windows takes the Win32 one whole: overlapped I/O is the
# kernel's own, so there is nothing left running inside a library a frontend
# unloads. Everyone else takes the POSIX seam's byte transport — glibc's POSIX
# AIO would still have helper threads holding a buffer inside a library that is
# going away.
if(WIN32)
    include(${RP6502_SRC}/osal/windows/windows.cmake)
    rp6502_osal_windows(emu_core)
else()
    include(${RP6502_SRC}/osal/posix/posix.cmake)
    rp6502_osal_posix(emu_core TRANSPORT sync)
    target_sources(emu_core PRIVATE ${RP6502_OSAL}/os.c)
endif()

# Everything that reaches a shared object has to be relocatable, emu_core
# included — it is a static library only on the way in.
set_property(TARGET emu_core PROPERTY POSITION_INDEPENDENT_CODE ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)


# rp6502_libretro, exactly: the buildbot builds ${CORENAME}_libretro and the
# frontend loads rp6502_libretro.so, so the target name is the file name and
# PREFIX is what keeps the "lib" off.
add_library(rp6502_libretro SHARED
    ${CMAKE_CURRENT_LIST_DIR}/retro.c
    ${CMAKE_CURRENT_LIST_DIR}/input.c
    ${RP6502_SRC}/core/sys/crc32.c
    ${RP6502_SRC}/core/sys/version.c)
set_target_properties(rp6502_libretro PROPERTIES PREFIX "")
target_link_libraries(rp6502_libretro PRIVATE emu_core)
target_include_directories(rp6502_libretro PRIVATE ${RP6502_LIBRETRO_INCLUDE})
target_include_directories(emu_core PUBLIC ${RP6502_LIBRETRO_INCLUDE})

# The ABI is the entire export surface. libretro.h marks the retro_*
# definitions visible itself, so hiding everything by default leaves exactly
# them; each format then says so a second time, where a toolchain would
# otherwise re-export what it inlined. MSVC needs none of this — dllexport
# is the whole mechanism there and nothing is exported without it.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(emu_core PRIVATE -fvisibility=hidden)
    target_compile_options(rp6502_libretro PRIVATE -fvisibility=hidden)
    if(APPLE)
        # ld64 takes an export list, not a version script.
        target_link_options(rp6502_libretro PRIVATE
            -Wl,-exported_symbols_list,${CMAKE_CURRENT_LIST_DIR}/link_apple.exp)
        set_target_properties(rp6502_libretro PROPERTIES
            LINK_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/link_apple.exp)
    elseif(WIN32)
        # A PE export table is the dllexports libretro.h already wrote, and
        # their presence is what turns off MinGW ld's export-everything
        # default — so there is nothing left to hide and no script to say it
        # with. -static-libgcc keeps the DLL from wanting a runtime nobody
        # who downloads a core has.
        target_link_options(rp6502_libretro PRIVATE
            -Wl,--no-undefined -static-libgcc)
    else()
        target_link_options(rp6502_libretro PRIVATE
            -Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/link.T
            -Wl,--no-undefined)
        set_target_properties(rp6502_libretro PROPERTIES
            LINK_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/link.T)
    endif()
endif()

rp6502_use_version_header(rp6502_libretro ${RP6502_SRC}/core/sys/version.c emu_core)

# The suite needs python3 and the generated ROM corpus, which a buildbot
# building a core to ship has no use for. Every other root builds its tests
# unconditionally because every other root is somebody's working tree; this
# one is also read by strangers' CI.
option(RP6502_TESTS "Build the test suite" ON)
if(RP6502_TESTS)
    enable_testing()
    # What a C compiler can answer: this core's own claims, and the machine's.
    include(${RP6502_ROOT}/tests/rp6502_tests.cmake)
    add_subdirectory(${RP6502_ROOT}/tests/host/libretro tests/host/libretro)
    add_subdirectory(${RP6502_ROOT}/tests/cpu tests/cpu)
endif()
