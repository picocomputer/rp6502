# The emulator on Windows.

cmake_minimum_required(VERSION 3.20)

project(rp6502-emu-win C CXX)

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
# No POSIX seam here: this host answers all four of them itself.
target_sources(emu_core PRIVATE dir.c fs.c host.c win.c)

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
# hid for the raw-HID gamepads; the XInput runtime is LoadLibrary'd because
# its DLL name varies and the Guide button is only at an ordinal.
target_link_libraries(rp6502-emu PRIVATE emu_core d3d11 dxgi shell32 hid user32)
target_compile_definitions(rp6502-emu PRIVATE
    SOKOL_NO_ENTRY SOKOL_D3D11 RP6502_PAD_HOST)

# GUI subsystem so no console pops up; keep our main() (host_console_attach
# reattaches stdio when launched from a terminal).
if(MSVC)
    target_link_options(rp6502-emu PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
else()
    target_link_options(rp6502-emu PRIVATE -mwindows)
endif()

# Windows is the only platform without getopt
rp6502_submodule(vendor/wingetopt SENTINEL src/getopt.c
    WANTS "getopt on Windows")
add_subdirectory(${RP6502_VENDOR}/wingetopt wingetopt)
target_link_libraries(rp6502-emu PRIVATE wingetopt)
target_compile_definitions(rp6502-emu PRIVATE _BSD_SOURCE)

rp6502_use_version_header(rp6502-emu ${RP6502_SRC}/core/emu/app/version.c)

# The version metadata Explorer reads. RC is not one of project()'s languages,
# so it is enabled where it is used; both rc.exe and windres accept this file.
enable_language(RC)
configure_file(version.rc.in version.rc @ONLY)
target_sources(rp6502-emu PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)

rp6502_emu_debugger(rp6502-emu)

enable_testing()
# What a C compiler can answer: this program's own tests, and the machine's.
# tests/cpu registers what needs a simulator only where one exists, so the
# same directory serves this tree and the fpga one.
include(${RP6502_ROOT}/tests/rp6502_tests.cmake)
add_subdirectory(${RP6502_ROOT}/tests/host/emu tests/host/emu)
add_subdirectory(${RP6502_ROOT}/tests/cpu tests/cpu)
