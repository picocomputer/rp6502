cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Project definitions for both RIA and VGA.
# Please change name for hardware forks.
# Creator flag hides MAC and SSID.
# RP6502_LOCALE is the build default locale.
set(RP6502_PROJECT_DEFINITIONS
    RP6502_NAME="Picocomputer 6502"
    RP6502_LOCALE=EN
    RP6502_EXFAT=0
    RP6502_CREATOR=0
)

# The shared src/ and vendor/ trees live beside this root CMakeLists (the
# firmware build). Reference sources and vendored deps via RP6502_ROOT.
get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

# The two the firmware reads, asked for here because the SDK is handed
# PICO_TINYUSB_PATH below and looks inside it during pico_sdk_init().
include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/tinyusb
    SENTINEL src/portable/raspberrypi/rp2040/rp2040_usb.c
    WANTS "the firmware's USB stack")
rp6502_submodule(vendor/littlefs SENTINEL lfs.c
    WANTS "the firmware's flash filesystem")

# Project subdirectories (under ${RP6502_ROOT}/src/mach/pico).
set(RP6502_PROJECT_DIRECTORIES
    ria
    vga
)

# Force wrappers defined by pico_printf and pico_stdlib to be
# preserved during link-time interprocedural optimizations.
set(IPO_PRINTF_LINK_OPTIONS
    -Wl,-u,__wrap_printf
    -Wl,-u,__wrap_vprintf
    -Wl,-u,__wrap_puts
    -Wl,-u,__wrap_putchar
    -Wl,-u,__wrap_getchar
    -Wl,-u,__wrap_sprintf
    -Wl,-u,__wrap_snprintf
    -Wl,-u,__wrap_vsnprintf
)

include(${RP6502_ROOT}/version.cmake)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_THREAD_LOCAL_TYPE emutls CACHE INTERNAL "")

include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

set(PICO_TINYUSB_PATH ${RP6502_ROOT}/vendor/tinyusb)

project(RP6502 C CXX ASM)

pico_sdk_init()

foreach(_dir IN LISTS RP6502_PROJECT_DIRECTORIES)
    add_subdirectory(${RP6502_ROOT}/src/mach/pico/${_dir} ${_dir})
endforeach()
