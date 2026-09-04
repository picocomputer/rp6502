cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The firmware build, and the file the Raspberry Pi Pico VS Code extension
# owns: it reads the versions and the board out of this file by name, and
# rewrites the marked block below when you switch SDK. What the firmware is
# made of is in src/host/pico, which this ends by adding.
get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

# TinyUSB is asked for here because the SDK is handed PICO_TINYUSB_PATH below
# and looks inside it during pico_sdk_init().
include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/tinyusb
    SENTINEL src/portable/raspberrypi/rp2040/rp2040_usb.c
    WANTS "the firmware's USB stack")

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

add_subdirectory(${RP6502_ROOT}/src/host/pico pico)
