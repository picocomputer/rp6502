#
# Copyright (c) 2026 Rumbledethumps
# SPDX-License-Identifier: BSD-3-Clause
#
# RP6502 emulator. Builds the desktop emulator in src/emu using floooh's
# chips (65C02 core, vendored+extended as src/emu/w65c02.h) and sokol for
# windowing/graphics. The vendored vga/term renderer is compiled directly
# against the shim headers in src/emu/shim.
#

cmake_minimum_required(VERSION 3.20)
# C CXX: the core + vendored firmware are C; the debugger (cppdap adapter, the
# chips/ImGui UI) is C++. CXX is enabled now so those units link in later phases.
project(rp6502-emu C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Emit compile_commands.json so editors (clangd / VS Code C/C++) get the exact
# include order and defines — the shim-before-firmware order matters for
# IntelliSense to resolve sys/, scanvideo/, and pico/ correctly.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RP6502_EMU_BUILD_TESTS "Build the CTest test suite" ON)

get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
set(SRC ${RP6502_ROOT}/src)
set(EMU ${RP6502_ROOT}/src/emu)
set(RP6502 ${RP6502_ROOT}/src)
set(VENDOR ${RP6502_ROOT}/vendor)
set(CHIPS ${VENDOR}/chips)
set(SOKOL ${VENDOR}/sokol)
set(IMGUI ${VENDOR}/imgui)

# ---------------------------------------------------------------------------
# emu_core: the headless machine (CPU + RIA + XRAM + syscalls + term render).
# No window, no main(); both the app and the tests link against this.
# ---------------------------------------------------------------------------
add_library(emu_core STATIC
    ${EMU}/sys/mem.c
    ${EMU}/sys/ria.c
    ${EMU}/host/host.c
    ${EMU}/dbg/dbg.c # debugger core: run/stop/step + breakpoints (inert until active)
    ${EMU}/sys/vga.c
    ${EMU}/sys/xreg.c
    ${EMU}/msc/mscpath.c
    ${EMU}/msc/msc.c
    ${EMU}/msc/mscdir.c
    ${EMU}/mon/install.c
    ${EMU}/mon/rom.c
    ${EMU}/host/png.c
    ${EMU}/sys/sys.c
    ${EMU}/sys/w65c02.c
    ${EMU}/api/api.c
    ${EMU}/api/oem.c
    ${EMU}/host/rand.c
    ${EMU}/api/std.c
    ${EMU}/api/dir.c
    ${EMU}/sys/com.c
    ${EMU}/hid/kbd.c
    ${EMU}/hid/pad.c
    ${EMU}/hid/mou.c
    ${EMU}/sys/via.c # 6522 VIA via chips m6522.h (CHIPS_IMPL in this TU)
    ${EMU}/api/clk.c
    ${EMU}/api/pro.c
    ${EMU}/aud/snd.c
    ${EMU}/aud/aud.c # emulator stand-in for ria/aud/aud.c (PWM/IRQ -> host pump)
    # vendored RP6502 audio drivers (compiled verbatim; their only hardware
    # touch is pwm_set_chan_level, captured by snd.c via shim/hardware/pwm.h)
    ${RP6502}/ria/aud/bel.c
    ${RP6502}/ria/aud/psg.c
    ${RP6502}/ria/aud/opl.c
    ${VENDOR}/emu8950/emu8950.c
    # vendored FatFs Unicode tables (ff_uni2oem) for OEM code-page conversion
    ${VENDOR}/fatfs/ffunicode.c
    # vendored RP6502 terminal renderer (compiled with the shim headers)
    ${RP6502}/vga/term/term.c
    ${RP6502}/vga/term/font.c
    ${RP6502}/vga/term/color.c
    # vendored RP6502 graphics-mode scanline renderers
    ${RP6502}/vga/modes/mode1.c
    ${RP6502}/vga/modes/mode2.c
    ${RP6502}/vga/modes/mode3.c
    ${RP6502}/vga/modes/mode4.c
    ${RP6502}/vga/modes/mode5.c
    # vendored RP6502 attribute handler (ria_get/set_attr + legacy phi2/
    # codepage/lrand/errno_opt); the host bits it reaches are provided by the
    # emulator's sys/com/pro/ria/oem/clk modules plus rand.c
    ${RP6502}/ria/api/atr.c
    # vendored RP6502 line editor for stdin (compiled with the shim headers)
    ${RP6502}/ria/str/rln.c
)

# The vendored firmware targets the 32-bit RP2350, where pointers are 32-bit;
# its alignment checks cast pointers to uint32_t. Harmless on a 64-bit host (the
# low bits survive truncation), so silence the warning for those files only.
set_source_files_properties(
    ${RP6502}/vga/modes/mode1.c
    ${RP6502}/vga/modes/mode2.c
    ${RP6502}/vga/modes/mode3.c
    PROPERTIES COMPILE_OPTIONS "-Wno-pointer-to-int-cast"
)

# emu8950.c gates its whole body on USE_EMU8950_OPL (the firmware sets it on the
# RIA target); without it the OPL2 core compiles to nothing.
set_source_files_properties(
    ${VENDOR}/emu8950/emu8950.c
    PROPERTIES COMPILE_DEFINITIONS "USE_EMU8950_OPL=1"
)

# Header resolution order (PUBLIC so the app and tests inherit it):
#   1. ${EMU}/shim   — our sys/, hardware/, pico/, scanvideo/ stand-ins win over
#                      the firmware/SDK originals the vendored TUs include.
#   2. the firmware tree — "api/*.h", "sys/cpu.h", "aud/*.h", "str/rln.h" etc.
#                      resolve to ria/ (and term/, modes/ to vga/).
#   3. ${SRC}        — the emulator's OWN headers, namespaced as "emu/<sub>/<mod>.h",
#                      mirroring the firmware folders (api/ sys/ hid/ aud/ mon/ …)
#                      without colliding with the firmware paths above.
# The emulator never includes its own headers bare — always "emu/<sub>/<mod>.h" — so
# src/emu itself is NOT on the path; that is what lets src/emu/api/, src/emu/sys/ etc.
# coexist with the firmware's api/, sys/ without shadowing the headers the vendored
# code needs (and the current-directory rule then can't shadow them either).
target_include_directories(emu_core PUBLIC
    ${EMU}/shim
    ${RP6502}/vga
    ${RP6502}/ria
    ${VENDOR}/fatfs
    ${RP6502}
    ${VENDOR}
    ${SRC}
    ${CHIPS}/chips
    ${CHIPS}
    ${SOKOL}
)
target_compile_definitions(emu_core PUBLIC _GNU_SOURCE)
target_link_libraries(emu_core PUBLIC m)

# ---------------------------------------------------------------------------
# rp6502-emu: the desktop app (argv + headless screenshot + sokol window).
# ---------------------------------------------------------------------------
include(CheckIncludeFile)
if(WIN32 OR APPLE OR EMSCRIPTEN)
    set(EMU_HAS_WINDOW ON)
else()
    check_include_file("GL/gl.h" EMU_HAS_GL_HEADER)
    set(EMU_HAS_WINDOW ${EMU_HAS_GL_HEADER})
endif()

# Audio output needs a window (saudio is fed from the app's frame loop) plus a
# backend: WASAPI/CoreAudio/WebAudio ship with those platforms, Linux uses ALSA
# (its dev headers). The audio *core* (emu_core) always builds; this only gates
# the host playback path.
if(EMU_HAS_WINDOW)
    if(WIN32 OR APPLE OR EMSCRIPTEN)
        set(EMU_HAS_AUDIO ON)
    else()
        check_include_file("alsa/asoundlib.h" EMU_HAS_ALSA_HEADER)
        set(EMU_HAS_AUDIO ${EMU_HAS_ALSA_HEADER})
    endif()
endif()

set(APP_SOURCES ${EMU}/app/main.c ${EMU}/app/cli.c ${EMU}/app/app_sokol.c)
if(EMU_HAS_WINDOW)
    list(APPEND APP_SOURCES ${EMU}/app/sokol_impl.c)
endif()
# Host audio playback (resampler -> sokol-audio): only when an audio backend was
# found. The synth itself lives in emu_core (snd.c/aud.c); this is the window-side
# output path, so it joins the app, not emu_core.
if(EMU_HAS_AUDIO)
    list(APPEND APP_SOURCES ${EMU}/app/audio_out.c)
endif()

add_executable(rp6502-emu ${APP_SOURCES})
target_link_libraries(rp6502-emu PRIVATE emu_core)
target_include_directories(rp6502-emu PRIVATE ${SOKOL})

# main.c parses the command line with getopt_long. Linux (glibc/musl), macOS, and
# Emscripten ship it in libc; Windows/MSVC does not, so vendor alex85k/wingetopt
# (small, BSD-licensed) there only. Its PUBLIC include dir propagates getopt.h via
# the link. _BSD_SOURCE exposes wingetopt's optreset, used to reset getopt state
# between the command-line and ROM-asset parse passes.
if(WIN32)
    add_subdirectory(${VENDOR}/wingetopt wingetopt)
    target_link_libraries(rp6502-emu PRIVATE wingetopt)
    target_compile_definitions(rp6502-emu PRIVATE _BSD_SOURCE)
endif()

if(NOT EMU_HAS_WINDOW)
    message(WARNING
        "GPU dev headers not found: building rp6502-emu WITHOUT a live window. "
        "It still runs ROMs and renders via --screenshot <file.png>. "
        "For the window install the GL/X11 dev headers, e.g. on Debian/Ubuntu: "
        "sudo apt-get install libgl-dev libx11-dev libxi-dev libxcursor-dev")
else()
    # SOKOL_NO_ENTRY: we provide main() ourselves and call sapp_run() directly,
    # so sokol must not emit its own entry point / expect sokol_main().
    target_compile_definitions(rp6502-emu PRIVATE EMU_WITH_SOKOL SOKOL_NO_ENTRY)
    # SOKOL_IMPL only in the dedicated implementation unit.
    set_source_files_properties(${EMU}/app/sokol_impl.c PROPERTIES COMPILE_DEFINITIONS SOKOL_IMPL)
    if(WIN32)
        target_compile_definitions(rp6502-emu PRIVATE SOKOL_D3D11)
        target_link_libraries(rp6502-emu PRIVATE d3d11 dxgi)
    elseif(APPLE)
        # sokol on macOS needs Objective-C; compile the impl units as .m and
        # link the Cocoa/Metal frameworks. (macOS bring-up pass.)
        target_compile_definitions(rp6502-emu PRIVATE SOKOL_METAL)
        set_source_files_properties(${EMU}/app/sokol_impl.c ${EMU}/app/app_sokol.c
            PROPERTIES COMPILE_FLAGS "-x objective-c")
        target_link_libraries(rp6502-emu PRIVATE
            "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework MetalKit")
        if(EMU_HAS_AUDIO)
            target_link_libraries(rp6502-emu PRIVATE "-framework AudioToolbox")
        endif()
    elseif(EMSCRIPTEN)
        target_compile_definitions(rp6502-emu PRIVATE SOKOL_GLES3)
        target_link_options(rp6502-emu PRIVATE
            "-sUSE_WEBGL2=1"
            "-sALLOW_MEMORY_GROWTH=1"
            "-sSTACK_SIZE=512KB"
            "-sFORCE_FILESYSTEM=1"
            "-lidbfs.js"
            "-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,callMain,addRunDependency,removeRunDependency")
        set_target_properties(rp6502-emu PROPERTIES
            OUTPUT_NAME rp6502
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/html")
        # Assemble the web roots
        file(GLOB WEB_ROMS "${CMAKE_SOURCE_DIR}/tests/roms/*.rp6502")
        add_custom_target(web_root ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${EMU}/web" "${CMAKE_BINARY_DIR}/html"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/html/roms"
            COMMAND ${CMAKE_COMMAND} -E copy ${WEB_ROMS} "${CMAKE_BINARY_DIR}/html/roms"
            COMMENT "Assembling deployable web root (build/web/html) + test ROMs"
            VERBATIM)
        add_dependencies(web_root rp6502-emu)
    else() # Linux / BSD
        find_package(Threads REQUIRED)
        target_compile_definitions(rp6502-emu PRIVATE SOKOL_GLCORE)
        target_link_libraries(rp6502-emu PRIVATE
            X11 Xi Xcursor GL dl Threads::Threads)
        if(EMU_HAS_AUDIO)
            target_link_libraries(rp6502-emu PRIVATE asound)
        endif()
    endif()
    # EMU_WITH_AUDIO compiles in the saudio playback path (sokol_impl emits the
    # backend, app_sokol pushes resampled samples). Off -> a silent window.
    if(EMU_HAS_AUDIO)
        target_compile_definitions(rp6502-emu PRIVATE EMU_WITH_AUDIO)
    else()
        message(WARNING
            "Audio backend not found: building rp6502-emu WITHOUT sound. "
            "On Debian/Ubuntu: sudo apt-get install libasound2-dev")
    endif()

    # -----------------------------------------------------------------------
    # Debugger overlay (non-WASM): the chips ImGui debugger (ui_dbg/ui_m6502/
    # ui_m6522) rendered via sokol_imgui in C++ mode against Dear ImGui. Shown
    # only when the emulator runs in debug mode (--debug / --dap). The cppdap
    # DAP server (dap.cpp) is added to this block in a later phase.
    # -----------------------------------------------------------------------
    if(NOT EMSCRIPTEN)
        # cppdap (the DAP server library): no examples/tests/fuzzer, vendored
        # nlohmann/json. Exposes the `cppdap` target with its include dir.
        set(CPPDAP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(CPPDAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(CPPDAP_BUILD_FUZZER OFF CACHE BOOL "" FORCE)
        add_subdirectory(${VENDOR}/cppdap cppdap)

        target_sources(rp6502-emu PRIVATE
            ${EMU}/dbg/imgui_impl.cc
            ${EMU}/dbg/dbgui.cc
            ${EMU}/dbg/dbgui_layout.cc # [EMU]-section layout persistence (no CHIPS_UI_IMPL)
            ${EMU}/dbg/dap.cpp
            ${EMU}/dbg/dwarf_line.c
            ${EMU}/dbg/dwarf_info.c
            ${EMU}/dbg/cc65dbg.c
            ${IMGUI}/imgui.cpp
            ${IMGUI}/imgui_draw.cpp
            ${IMGUI}/imgui_tables.cpp
            ${IMGUI}/imgui_widgets.cpp)
        target_include_directories(rp6502-emu PRIVATE ${IMGUI})
        target_compile_definitions(rp6502-emu PRIVATE EMU_WITH_DEBUGGER UI_DBG_USE_M6502)
        target_link_libraries(rp6502-emu PRIVATE cppdap)
        # CHIPS_UI_IMPL / SOKOL_IMGUI_IMPL each belong to exactly one TU.
        set_source_files_properties(${EMU}/dbg/imgui_impl.cc
            PROPERTIES COMPILE_DEFINITIONS "SOKOL_IMGUI_IMPL")
        set_source_files_properties(${EMU}/dbg/dbgui.cc
            PROPERTIES COMPILE_DEFINITIONS "CHIPS_UI_IMPL")
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(RP6502_EMU_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_subdirectory(tests)
endif()
