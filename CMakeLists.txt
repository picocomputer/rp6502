cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(RP6502_ROOT ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

# The TinyUSB submodule has to be present before pico_sdk_init(), because the
# SDK reads the tree at PICO_TINYUSB_PATH during that call.
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

project(rp6502-pico C CXX ASM)

# TinyUSB's hw/bsp/rp2040/family.cmake sets CFG_TUSB_DEBUG from LOG when
# pico_sdk_init() loads it, so LOG has to be set before that call.
include(${RP6502_ROOT}/src/core/log.cmake)
rp6502_log_level_of(tinyusb RP6502_TINYUSB_LEVEL)
if(RP6502_TINYUSB_LEVEL STREQUAL "DEBUG" AND NOT DEFINED LOG)
    set(LOG 3)
endif()

pico_sdk_init()

add_subdirectory(${RP6502_ROOT}/src/host/pico pico)
