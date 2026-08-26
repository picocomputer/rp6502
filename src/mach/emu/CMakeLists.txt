# The emulator on the desktops it runs natively: Linux and the BSDs, macOS,
# Windows.
#
# One root for the three because they are one program. What differs is the
# host directory beside this one, the graphics backend sokol compiles, and
# what the linker is handed — each in a branch below and nowhere else.

cmake_minimum_required(VERSION 3.21)

project(rp6502-emu C CXX)

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

# The host this build is for. RP6502_HOST leads emu_core's include path, so
# `#include "host.h"` anywhere is this directory's.
if(WIN32)
    set(RP6502_HOST ${RP6502_SRC}/host/windows)
elseif(APPLE)
    set(RP6502_HOST ${RP6502_SRC}/host/macos)
else()
    set(RP6502_HOST ${RP6502_SRC}/host/linux)
endif()

include(${RP6502_SRC}/core/emu.cmake)
if(WIN32)
    include(${RP6502_SRC}/host/windows/windows.cmake)
    # The Win32 seam is host/windows/host.c, shared with the libretro core.
    # This is the emulator's own half: a frame pacer, a console and an ANSI
    # main(), none of which a core has.
    target_sources(emu_core PRIVATE ${RP6502_HOST}/host_emu.c)
else()
    include(${RP6502_SRC}/host/posix/posix.cmake)
    target_sources(emu_core PRIVATE ${RP6502_HOST}/host.c)
endif()

# One optimization unit in Release, as it was when this tree was one file:
# emu_core was made above this line, so it is told, and everything made below
# inherits the directory variable.
set_property(TARGET emu_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})

include(${RP6502_SRC}/host/sokol/sokol.cmake)

# The desktop window + audio stack. Windows and macOS carry their own
# (D3D11/Metal); Linux/BSD needs GL + X11 (+ Xi/Xcursor) and ALSA. These are
# hard requirements — a windowless or silent emulator ships nowhere — so a
# missing dev package fails configuration instead of degrading the build.
if(NOT WIN32 AND NOT APPLE)
    find_package(OpenGL)
    find_package(X11)
    find_package(ALSA)
    if(NOT OPENGL_FOUND OR NOT X11_FOUND OR NOT X11_Xi_FOUND OR NOT X11_Xcursor_FOUND OR NOT ALSA_FOUND)
        message(FATAL_ERROR
            "rp6502-emu needs the desktop dev stack. On Debian/Ubuntu:\n"
            "  sudo apt-get install libgl-dev libx11-dev libxi-dev libxcursor-dev libasound2-dev")
    endif()
    find_package(Threads REQUIRED)
endif()

add_executable(rp6502-emu
    ${RP6502_EMU_APP}
    ${RP6502_SRC}/host/sokol/main.c
    ${RP6502_EMU_WINDOW}
    ${RP6502_HOST}/window.c
    ${RP6502_HOST}/gamepad.c
    ${RP6502_SRC}/host/sokol/gamepad_input.c)
target_link_libraries(rp6502-emu PRIVATE emu_core)
target_compile_definitions(rp6502-emu PRIVATE SOKOL_NO_ENTRY RP6502_PAD_HOST)

if(WIN32)
    target_compile_definitions(rp6502-emu PRIVATE SOKOL_D3D11 _BSD_SOURCE)
    # hid for the raw-HID gamepads; the XInput runtime is LoadLibrary'd because
    # its DLL name varies and the Guide button is only at an ordinal.
    target_link_libraries(rp6502-emu PRIVATE d3d11 dxgi shell32 hid user32)
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
elseif(APPLE)
    target_compile_definitions(rp6502-emu PRIVATE SOKOL_METAL)
    # sokol on macOS needs Objective-C; compile the impl units as .m and
    # link the Cocoa/Metal frameworks. (macOS bring-up pass.)
    set_source_files_properties(
        ${RP6502_SRC}/host/sokol/sokol_impl.c
        ${RP6502_SRC}/host/sokol/window_core.c
        ${RP6502_SRC}/host/sokol/input.c
        ${RP6502_HOST}/window.c
        ${RP6502_HOST}/gamepad.c
        PROPERTIES COMPILE_FLAGS "-x objective-c")
    # GameController rather than IOKit: Apple's own drivers normalize every
    # modern gamepad, and it asks the user for no permission to do it.
    target_link_libraries(rp6502-emu PRIVATE
        "-framework Cocoa" "-framework QuartzCore" "-framework Metal"
        "-framework MetalKit" "-framework AudioToolbox" "-framework GameController")
else()
    target_compile_definitions(rp6502-emu PRIVATE SOKOL_GLCORE)
    target_link_libraries(rp6502-emu PRIVATE
        OpenGL::GL X11::X11 X11::Xi X11::Xcursor ALSA::ALSA
        ${CMAKE_DL_LIBS} Threads::Threads)
endif()

rp6502_use_version_header(rp6502-emu ${RP6502_SRC}/core/sys/version.c)

if(WIN32)
    # The version metadata Explorer reads. RC is not one of project()'s
    # languages, so it is enabled where it is used; both rc.exe and windres
    # accept this file.
    enable_language(RC)
    configure_file(version.rc.in version.rc @ONLY)
    target_sources(rp6502-emu PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
endif()

rp6502_emu_debugger(rp6502-emu)

enable_testing()
# What a C compiler can answer: this program's own tests, and the machine's.
# tests/cpu registers what needs a simulator only where one exists, so the
# same directory serves this tree and the fpga one.
include(${RP6502_ROOT}/tests/rp6502_tests.cmake)
add_subdirectory(${RP6502_ROOT}/tests/host/emu tests/host/emu)
add_subdirectory(${RP6502_ROOT}/tests/cpu tests/cpu)
