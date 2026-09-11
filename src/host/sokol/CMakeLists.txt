cmake_minimum_required(VERSION 3.21)

project(rp6502-emu C CXX)

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

include(${RP6502_ROOT}/version.cmake)

if(WIN32)
    set(RP6502_OSAL ${RP6502_SRC}/osal/windows)
    set(RP6502_SOKOL_OS ${CMAKE_CURRENT_LIST_DIR}/windows)
elseif(APPLE)
    set(RP6502_OSAL ${RP6502_SRC}/osal/macos)
    set(RP6502_SOKOL_OS ${CMAKE_CURRENT_LIST_DIR}/macos)
else()
    set(RP6502_OSAL ${RP6502_SRC}/osal/linux)
    set(RP6502_SOKOL_OS ${CMAKE_CURRENT_LIST_DIR}/linux)
endif()

include(${RP6502_SRC}/core/emu.cmake)
if(WIN32)
    include(${RP6502_SRC}/osal/windows/windows.cmake)
    rp6502_osal_windows(emu_core TRANSPORT overlapped)
    target_sources(emu_core PRIVATE ${RP6502_SRC}/osal/windows/console.c)
else()
    include(${RP6502_SRC}/osal/posix/posix.cmake)
    rp6502_osal_posix(emu_core TRANSPORT aio)
    target_sources(emu_core PRIVATE ${RP6502_OSAL}/os.c
        ${RP6502_SRC}/osal/posix/console.c)
endif()

target_include_directories(emu_core PUBLIC ${CMAKE_CURRENT_LIST_DIR})

include(${RP6502_SRC}/host/sokol/sokol.cmake)

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
    ${RP6502_SRC}/host/sokol/cli/main.c
    ${RP6502_SRC}/host/sokol/cli/streams.c
    ${RP6502_SRC}/host/sokol/cli/console.c
    ${RP6502_SRC}/host/sokol/cli/log.c
    ${RP6502_EMU_WINDOW}
    ${RP6502_SOKOL_OS}/entry.c
    ${RP6502_SOKOL_OS}/gamepad.c
    ${RP6502_SRC}/host/sokol/app/gamepad.c)
target_link_libraries(rp6502-emu PRIVATE emu_core)
target_compile_definitions(rp6502-emu PRIVATE SOKOL_NO_ENTRY RP6502_PAD_HOST)

if(WIN32)
    target_compile_definitions(rp6502-emu PRIVATE SOKOL_D3D11 _BSD_SOURCE)
    # The XInput runtime is not linked here because windows/gamepad.c loads it
    # by name at run time.
    target_link_libraries(rp6502-emu PRIVATE d3d11 dxgi shell32 hid user32)
    # The GUI subsystem keeps Windows from opening a console for the program,
    # and mainCRTStartup keeps main() as the entry point rather than the
    # WinMain the GUI subsystem would otherwise want.
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
    # macos/gamepad.c imports GameController, and sokol's macOS backends are
    # Objective-C, so these are compiled as Objective-C rather than C.
    set_source_files_properties(
        ${RP6502_SRC}/host/sokol/app/sokol.c
        ${RP6502_SRC}/host/sokol/app/app.c
        ${RP6502_SRC}/host/sokol/app/prompt.c
        ${RP6502_SRC}/host/sokol/app/gfx.c
        ${RP6502_SRC}/host/sokol/app/input.c
        ${RP6502_SOKOL_OS}/entry.c
        ${RP6502_SOKOL_OS}/gamepad.c
        PROPERTIES COMPILE_FLAGS "-x objective-c")
    target_link_libraries(rp6502-emu PRIVATE
        "-framework Cocoa" "-framework QuartzCore" "-framework Metal"
        "-framework MetalKit" "-framework AudioToolbox" "-framework GameController")
else()
    target_compile_definitions(rp6502-emu PRIVATE SOKOL_GLCORE)
    target_link_libraries(rp6502-emu PRIVATE
        OpenGL::GL X11::X11 X11::Xi X11::Xcursor ALSA::ALSA
        ${CMAKE_DL_LIBS} Threads::Threads)
endif()

rp6502_use_version_header(rp6502-emu ${RP6502_SRC}/core/sys/version.c emu_core)

if(WIN32)
    # RC is not one of project()'s languages, and CMake enables a language for
    # a directory rather than for a target, so it is enabled here. Both rc.exe
    # and windres accept the file.
    enable_language(RC)
    configure_file(${RP6502_SOKOL_OS}/version.rc.in version.rc @ONLY)
    target_sources(rp6502-emu PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
endif()

if(APPLE)
    # MACOSX_BUNDLE_INFO_PLIST substitutes only CMake's own MACOSX_BUNDLE_*
    # variables and the version fields here are ours, so the plist is
    # configured before it is handed over.
    configure_file(${RP6502_SOKOL_OS}/Info.plist.in Info.plist @ONLY)
    set_target_properties(rp6502-emu PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
endif()

rp6502_emu_debugger(rp6502-emu)

enable_testing()
include(${RP6502_ROOT}/tests/rp6502_tests.cmake)
add_subdirectory(${RP6502_ROOT}/tests/host/emu tests/host/emu)
add_subdirectory(${RP6502_ROOT}/tests/cpu tests/cpu)
# The tests under tests/pico drive a real board on a serial port.
if(RP6502_DEVICE)
    add_subdirectory(${RP6502_ROOT}/tests/pico tests/pico)
endif()
