cmake_minimum_required(VERSION 3.20)

project(rp6502-emu C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Emit compile_commands.json

option(RP6502_EMU_BUILD_TESTS "Build the CTest test suite" ON)

get_filename_component(RP6502_SRC ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
get_filename_component(RP6502_VENDOR ${CMAKE_CURRENT_LIST_DIR}/../../vendor ABSOLUTE)

add_library(emu_core STATIC
    ${RP6502_SRC}/emu/sys/mem.c
    ${RP6502_SRC}/emu/sys/ria.c
    ${RP6502_SRC}/emu/host/host.c
    ${RP6502_SRC}/emu/dbg/dbg.c
    ${RP6502_SRC}/emu/sys/vga.c
    ${RP6502_SRC}/emu/sys/xreg.c
    ${RP6502_SRC}/emu/host/dir.c
    ${RP6502_SRC}/emu/host/posixdir.c
    ${RP6502_SRC}/emu/host/fs.c
    ${RP6502_SRC}/emu/mon/install.c
    ${RP6502_SRC}/emu/mon/rom.c
    ${RP6502_SRC}/emu/host/png.c
    ${RP6502_SRC}/emu/sys/sys.c
    ${RP6502_SRC}/emu/chips/w65c02.c
    ${RP6502_SRC}/emu/api/api.c
    ${RP6502_SRC}/emu/api/oem.c
    ${RP6502_SRC}/emu/host/rand.c
    ${RP6502_SRC}/emu/api/std_drivers.c
    ${RP6502_SRC}/emu/sys/com.c
    ${RP6502_SRC}/emu/hid/kbd.c
    ${RP6502_SRC}/emu/hid/pad.c
    ${RP6502_SRC}/emu/hid/mou.c
    ${RP6502_SRC}/emu/sys/via.c
    ${RP6502_SRC}/emu/api/clk.c
    ${RP6502_SRC}/emu/api/pro.c
    ${RP6502_SRC}/emu/aud/snd.c
    ${RP6502_SRC}/emu/aud/aud.c
    ${RP6502_SRC}/ria/aud/bel.c
    ${RP6502_SRC}/ria/aud/psg.c
    ${RP6502_SRC}/ria/aud/opl.c
    ${RP6502_VENDOR}/emu8950/emu8950.c
    ${RP6502_VENDOR}/fatfs/ffunicode.c
    ${RP6502_VENDOR}/fatfs/ff.c
    ${RP6502_SRC}/ria/api/fat.c
    ${RP6502_SRC}/ria/api/dir.c
    ${RP6502_SRC}/ria/api/std.c
    ${RP6502_SRC}/ria/api/api.c
    ${RP6502_SRC}/emu/usb/msc.c
    ${RP6502_SRC}/vga/term/term.c
    ${RP6502_SRC}/vga/term/font.c
    ${RP6502_SRC}/vga/term/color.c
    ${RP6502_SRC}/vga/modes/mode1.c
    ${RP6502_SRC}/vga/modes/mode2.c
    ${RP6502_SRC}/vga/modes/mode3.c
    ${RP6502_SRC}/vga/modes/mode4.c
    ${RP6502_SRC}/vga/modes/mode5.c
    ${RP6502_SRC}/ria/api/atr.c
    ${RP6502_SRC}/ria/str/rln.c
)

# The vendored firmware targets the 32-bit RP2350, where pointers are 32-bit.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        ${RP6502_SRC}/vga/modes/mode1.c
        ${RP6502_SRC}/vga/modes/mode2.c
        ${RP6502_SRC}/vga/modes/mode3.c
        ${RP6502_SRC}/vga/modes/mode4.c
        ${RP6502_SRC}/vga/modes/mode5.c
        PROPERTIES COMPILE_OPTIONS "-Wno-pointer-to-int-cast"
    )
endif()

# emu8950.c gates its whole body on USE_EMU8950_OPL
set_source_files_properties(
    ${RP6502_VENDOR}/emu8950/emu8950.c
    PROPERTIES COMPILE_DEFINITIONS "USE_EMU8950_OPL=1"
)

# Header resolution order (PUBLIC so the app and tests inherit it):
target_include_directories(emu_core PUBLIC
    ${RP6502_SRC}/emu/shim
    ${RP6502_SRC}/vga
    ${RP6502_SRC}/ria
    ${RP6502_VENDOR}/fatfs
    ${RP6502_SRC}
    ${RP6502_VENDOR}
    ${RP6502_VENDOR}/chips/chips
    ${RP6502_VENDOR}/chips
    ${RP6502_VENDOR}/sokol
)
target_compile_definitions(emu_core PUBLIC _GNU_SOURCE RP6502_EXFAT=0)
target_link_libraries(emu_core PUBLIC m)

# Non-blocking file I/O (POSIX AIO) where the platform provides it; msc/rom fall
# back to synchronous read/write otherwise (web MEMFS, Windows).
include(CheckSymbolExists)
if(NOT EMSCRIPTEN)
    find_library(RT_LIBRARY rt)
    if(RT_LIBRARY)
        target_link_libraries(emu_core PUBLIC ${RT_LIBRARY})
        set(CMAKE_REQUIRED_LIBRARIES ${RT_LIBRARY})
    endif()
    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    check_symbol_exists(aio_read "aio.h" EMU_HAVE_AIO)
    unset(CMAKE_REQUIRED_DEFINITIONS)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(EMU_HAVE_AIO)
        target_compile_definitions(emu_core PRIVATE EMU_HAVE_AIO)
    endif()
endif()

# Determine if window available
include(CheckIncludeFile)
if(WIN32 OR APPLE OR EMSCRIPTEN)
    set(EMU_HAS_WINDOW ON)
else()
    check_include_file("GL/gl.h" EMU_HAS_GL_HEADER)
    check_include_file("X11/Xlib.h" EMU_HAS_X11_HEADER)
    check_include_file("X11/extensions/XInput2.h" EMU_HAS_XI_HEADER)
    check_include_file("X11/Xcursor/Xcursor.h" EMU_HAS_XCURSOR_HEADER)
    if(EMU_HAS_GL_HEADER AND EMU_HAS_X11_HEADER AND EMU_HAS_XI_HEADER AND EMU_HAS_XCURSOR_HEADER)
        set(EMU_HAS_WINDOW ON)
    else()
        set(EMU_HAS_WINDOW OFF)
    endif()
endif()

# Determine if audio available
if(EMU_HAS_WINDOW)
    if(WIN32 OR APPLE OR EMSCRIPTEN)
        set(EMU_HAS_AUDIO ON)
    else()
        check_include_file("alsa/asoundlib.h" EMU_HAS_ALSA_HEADER)
        set(EMU_HAS_AUDIO ${EMU_HAS_ALSA_HEADER})
    endif()
endif()

# Build source list with window and audio options
set(APP_SOURCES ${RP6502_SRC}/emu/app/main.c ${RP6502_SRC}/emu/app/cli.c ${RP6502_SRC}/emu/app/app_sokol.c)
if(EMU_HAS_WINDOW)
    list(APPEND APP_SOURCES ${RP6502_SRC}/emu/app/sokol_impl.c)
endif()
if(EMU_HAS_AUDIO)
    list(APPEND APP_SOURCES ${RP6502_SRC}/emu/app/audio_out.c)
endif()

add_executable(rp6502-emu ${APP_SOURCES})
target_link_libraries(rp6502-emu PRIVATE emu_core)
target_include_directories(rp6502-emu PRIVATE ${RP6502_VENDOR}/sokol)

# Windows is only platform without getopt
if(WIN32)
    add_subdirectory(${RP6502_VENDOR}/wingetopt wingetopt)
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
    # SOKOL_NO_ENTRY: we provide main() ourselves and call sapp_run() directly.
    target_compile_definitions(rp6502-emu PRIVATE EMU_WITH_SOKOL SOKOL_NO_ENTRY)
    set_source_files_properties(${RP6502_SRC}/emu/app/sokol_impl.c PROPERTIES COMPILE_DEFINITIONS SOKOL_IMPL)
    if(WIN32)
        target_compile_definitions(rp6502-emu PRIVATE SOKOL_D3D11)
        target_link_libraries(rp6502-emu PRIVATE d3d11 dxgi)
    elseif(APPLE)
        # sokol on macOS needs Objective-C; compile the impl units as .m and
        # link the Cocoa/Metal frameworks. (macOS bring-up pass.)
        target_compile_definitions(rp6502-emu PRIVATE SOKOL_METAL)
        set_source_files_properties(${RP6502_SRC}/emu/app/sokol_impl.c ${RP6502_SRC}/emu/app/app_sokol.c
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
        # Tester web root (build/web/html): the menu shell + every test ROM.
        file(GLOB WEB_ROMS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/roms/*.rp6502")
        add_custom_target(web_root ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${RP6502_SRC}/emu/html" "${CMAKE_BINARY_DIR}/html"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/html/roms"
            COMMAND ${CMAKE_COMMAND} -E copy ${WEB_ROMS} "${CMAKE_BINARY_DIR}/html/roms"
            COMMENT "Assembling tester web root (build/web/html) + all test ROMs"
            VERBATIM)
        add_dependencies(web_root rp6502-emu)
        # itch.io sample (build/web/itch.io): the single-program shell.
        add_custom_target(itch_root ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${RP6502_SRC}/emu/itch.io" "${CMAKE_BINARY_DIR}/itch.io"
            COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_SOURCE_DIR}/tests/roms/adventure.rp6502"
                "${CMAKE_BINARY_DIR}/html/rp6502.js"
                "${CMAKE_BINARY_DIR}/html/rp6502.wasm"
                "${CMAKE_BINARY_DIR}/itch.io"
            COMMENT "Assembling itch.io sample (build/web/itch.io) + adventure.rp6502"
            VERBATIM)
        add_dependencies(itch_root rp6502-emu)
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

    # Include debugging except on web version
    if(NOT EMSCRIPTEN)
        # cppdap (the DAP server library): no examples/tests/fuzzer, vendored
        # nlohmann/json. Exposes the `cppdap` target with its include dir.
        set(CPPDAP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(CPPDAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(CPPDAP_BUILD_FUZZER OFF CACHE BOOL "" FORCE)
        add_subdirectory(${RP6502_VENDOR}/cppdap cppdap)

        # GCC's -Warray-bounds mis-reads cppdap's dap::any scalar storage
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(cppdap PRIVATE -Wno-array-bounds)
        endif()

        target_sources(rp6502-emu PRIVATE
            ${RP6502_SRC}/emu/dbg/imgui_impl.cc
            ${RP6502_SRC}/emu/dbg/dbgui.cc
            ${RP6502_SRC}/emu/dbg/dbgui_layout.cc
            ${RP6502_SRC}/emu/dbg/dap.cpp
            ${RP6502_SRC}/emu/dbg/dwarf_line.c
            ${RP6502_SRC}/emu/dbg/dwarf_info.c
            ${RP6502_SRC}/emu/dbg/cc65dbg.c
            ${RP6502_VENDOR}/imgui/imgui.cpp
            ${RP6502_VENDOR}/imgui/imgui_draw.cpp
            ${RP6502_VENDOR}/imgui/imgui_tables.cpp
            ${RP6502_VENDOR}/imgui/imgui_widgets.cpp)
        target_include_directories(rp6502-emu PRIVATE ${RP6502_VENDOR}/imgui)
        target_compile_definitions(rp6502-emu PRIVATE EMU_WITH_DEBUGGER UI_DBG_USE_M6502)
        target_link_libraries(rp6502-emu PRIVATE cppdap)
        # CHIPS_UI_IMPL / SOKOL_IMGUI_IMPL each belong to exactly one TU.
        set_source_files_properties(${RP6502_SRC}/emu/dbg/imgui_impl.cc
            PROPERTIES COMPILE_DEFINITIONS "SOKOL_IMGUI_IMPL")
        set_source_files_properties(${RP6502_SRC}/emu/dbg/dbgui.cc
            PROPERTIES COMPILE_DEFINITIONS "CHIPS_UI_IMPL")
    endif()
endif()

# Tests
if(RP6502_EMU_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_subdirectory(tests)
endif()
