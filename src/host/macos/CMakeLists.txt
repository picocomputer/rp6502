# The emulator on macOS.

cmake_minimum_required(VERSION 3.20)

project(rp6502-emu-macos C CXX)

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
include(${RP6502_SRC}/core/posix/posix.cmake)
target_sources(emu_core PRIVATE host.c)

set_property(TARGET emu_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})

include(${RP6502_SRC}/core/emu/app/app.cmake)

add_executable(rp6502-emu
    ${RP6502_EMU_APP}
    ${RP6502_SRC}/core/emu/app/main.c
    ${RP6502_EMU_WINDOW}
    window.c
    pad.c
    ${RP6502_SRC}/core/emu/app/pad_input.c)
target_link_libraries(rp6502-emu PRIVATE emu_core)
target_compile_definitions(rp6502-emu PRIVATE
    SOKOL_NO_ENTRY SOKOL_METAL RP6502_PAD_HOST)

# sokol on macOS needs Objective-C; compile the impl units as .m and
# link the Cocoa/Metal frameworks. (macOS bring-up pass.)
set_source_files_properties(
    ${RP6502_SRC}/core/emu/app/sokol_impl.c
    ${RP6502_SRC}/core/emu/app/window_core.c
    ${RP6502_SRC}/core/emu/app/input.c
    window.c
    pad.c
    PROPERTIES COMPILE_FLAGS "-x objective-c")
# GameController rather than IOKit: Apple's own drivers normalize every
# modern pad, and it asks the user for no permission to do it.
target_link_libraries(rp6502-emu PRIVATE
    "-framework Cocoa" "-framework QuartzCore" "-framework Metal"
    "-framework MetalKit" "-framework AudioToolbox" "-framework GameController")

rp6502_use_version_header(rp6502-emu ${RP6502_SRC}/core/emu/app/version.c)

rp6502_emu_debugger(rp6502-emu)

enable_testing()
add_subdirectory(${RP6502_ROOT}/tests/emu tests)
