cmake_minimum_required(VERSION 3.13)

set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/src/pico-sdk)
set(PICO_EXTRAS_PATH ${CMAKE_CURRENT_LIST_DIR}/src/pico-extras)
# set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/src/tinyusb)

include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
add_subdirectory(${PICO_EXTRAS_PATH})

project(RP6502 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(src)
