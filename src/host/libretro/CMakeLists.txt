# The emulator as a libretro core, for RetroArch and the launchers that
# embed it.
#
# The thinnest host there is: no window, no audio device, no input loop, no
# main(). A frontend owns all four and calls retro_run once a frame. So this
# root takes emu_core and none of app.cmake — the sokol window, the command
# line, the script channel and the debugger are all things a core has no use
# for.

cmake_minimum_required(VERSION 3.20)

project(rp6502-emu-libretro C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Emit compile_commands.json

get_filename_component(RP6502_SRC ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
get_filename_component(RP6502_VENDOR ${RP6502_ROOT}/vendor ABSOLUTE)

# First: it takes RP6502_CI out of the cache, so nothing may read it earlier.
include(${RP6502_ROOT}/version.cmake)

set(RP6502_HOST ${CMAKE_CURRENT_LIST_DIR})
include(${RP6502_SRC}/core/emu/emu.cmake)

rp6502_submodule(vendor/libretro-common SENTINEL include/libretro.h
    SPARSE include WANTS "the libretro API header")
set(RP6502_LIBRETRO_INCLUDE ${RP6502_VENDOR}/libretro-common/include)

# The POSIX seam without posix.cmake, for its byte transport: a core is
# unloaded at the frontend's convenience, and glibc's POSIX AIO would still
# have helper threads holding a buffer inside a library that is going away.
target_sources(emu_core PRIVATE
    ${RP6502_SRC}/core/posix/dir.c
    ${RP6502_SRC}/core/posix/fs.c
    ${RP6502_SRC}/core/posix/fs_sync.c
    ${RP6502_SRC}/core/posix/host.c
    host.c)

# Everything that reaches a shared object has to be relocatable, emu_core
# included — it is a static library only on the way in.
set_property(TARGET emu_core PROPERTY POSITION_INDEPENDENT_CODE ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set_property(TARGET emu_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})

# rp6502_libretro, exactly: the buildbot builds ${CORENAME}_libretro and the
# frontend loads rp6502_libretro.so, so the target name is the file name and
# PREFIX is what keeps the "lib" off.
add_library(rp6502_libretro SHARED
    retro.c
    input.c
    ${RP6502_SRC}/core/emu/app/version.c)
set_target_properties(rp6502_libretro PROPERTIES PREFIX "")
target_link_libraries(rp6502_libretro PRIVATE emu_core)
target_include_directories(rp6502_libretro PRIVATE ${RP6502_LIBRETRO_INCLUDE})
target_include_directories(emu_core PUBLIC ${RP6502_LIBRETRO_INCLUDE})

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE AND NOT WIN32)
    # The ABI is the entire export surface. libretro.h marks the retro_*
    # definitions visible itself, so hiding everything by default leaves
    # exactly them, and the version script says so a second time where a
    # toolchain would otherwise re-export what it inlined.
    target_compile_options(emu_core PRIVATE -fvisibility=hidden)
    target_compile_options(rp6502_libretro PRIVATE -fvisibility=hidden)
    target_link_options(rp6502_libretro PRIVATE
        -Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/link.T
        -Wl,--no-undefined)
    set_target_properties(rp6502_libretro PROPERTIES
        LINK_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/link.T)
endif()

rp6502_use_version_header(rp6502_libretro ${RP6502_SRC}/core/emu/app/version.c)

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
