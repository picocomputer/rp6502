# The emulator on Linux and the BSDs.

cmake_minimum_required(VERSION 3.20)

project(rp6502-emu-linux C CXX)

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

# One optimization unit in Release, as it was when this tree was one file:
# emu_core was made above this line, so it is told, and everything made below
# inherits the directory variable.
set_property(TARGET emu_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${RP6502_EMU_IPO})

include(${RP6502_SRC}/core/emu/app/app.cmake)

# The desktop window + audio stack. Windows/macOS/web/Android carry their own
# (D3D11/Metal/WebGL/AAudio); Linux/BSD needs GL + X11 (+ Xi/Xcursor) and ALSA.
# These are hard requirements — a windowless or silent emulator ships nowhere —
# so a missing dev package fails configuration instead of degrading the build.
find_package(OpenGL)
find_package(X11)
find_package(ALSA)
if(NOT OPENGL_FOUND OR NOT X11_FOUND OR NOT X11_Xi_FOUND OR NOT X11_Xcursor_FOUND OR NOT ALSA_FOUND)
    message(FATAL_ERROR
        "rp6502-emu needs the desktop dev stack. On Debian/Ubuntu:\n"
        "  sudo apt-get install libgl-dev libx11-dev libxi-dev libxcursor-dev libasound2-dev")
endif()
find_package(Threads REQUIRED)

# A gamepad backend beside the window: sokol_app has no gamepad API, and unlike
# the browser and Android, which read pads their own way, nothing else would
# feed pad.c.
add_executable(rp6502-emu
    ${RP6502_EMU_APP}
    ${RP6502_SRC}/core/emu/app/main.c
    ${RP6502_EMU_WINDOW}
    window.c
    pad.c
    ${RP6502_SRC}/core/emu/app/pad_input.c)
target_link_libraries(rp6502-emu PRIVATE emu_core
    OpenGL::GL X11::X11 X11::Xi X11::Xcursor ALSA::ALSA
    ${CMAKE_DL_LIBS} Threads::Threads)
# SOKOL_NO_ENTRY: this host provides main() and calls sapp_run() directly.
target_compile_definitions(rp6502-emu PRIVATE
    SOKOL_NO_ENTRY SOKOL_GLCORE RP6502_PAD_HOST)

# On rp6502-emu rather than emu_core: the simulation links emu_core too, as its
# reference oracle, where a version is nothing to stamp.
rp6502_use_version_header(rp6502-emu ${RP6502_SRC}/core/emu/app/version.c)

rp6502_emu_debugger(rp6502-emu)

enable_testing()
# What a C compiler can answer: this program's own tests, and the machine's.
# tests/cpu registers what needs a simulator only where one exists, so the
# same directory serves this tree and the fpga one.
include(${RP6502_ROOT}/tests/rp6502_tests.cmake)
add_subdirectory(${RP6502_ROOT}/tests/host/emu tests/host/emu)
add_subdirectory(${RP6502_ROOT}/tests/cpu tests/cpu)
