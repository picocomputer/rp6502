# Old TinyUSB needed for Picoprobe

add_library(tinyusb_vga STATIC EXCLUDE_FROM_ALL)

target_include_directories(tinyusb_vga PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/tinyusb/src
    vga
)

file(GLOB_RECURSE TINY_USB_SRC_FILES ${CMAKE_CURRENT_LIST_DIR}/tinyusb/src/*.c)

target_link_libraries(tinyusb_vga PUBLIC
    pico_stdlib
)

target_sources(tinyusb_vga PRIVATE
    ${TINY_USB_SRC_FILES}
)


# The Pi Pico RIA

add_executable(rp6502_ria)
pico_add_extra_outputs(rp6502_ria)
pico_set_program_name(rp6502_ria "RP6502 RIA")
pico_set_binary_type(rp6502_ria copy_to_ram)

target_include_directories(rp6502_ria PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ria
)

target_compile_definitions(rp6502_ria PRIVATE
    LFS_NO_MALLOC=1
    LFS_NAME_MAX=16
)

target_compile_options(rp6502_ria PRIVATE
  -Wall -Wextra
)

target_link_libraries(rp6502_ria PRIVATE
    pico_stdlib
    pico_multicore
    pico_rand
    hardware_pio
    hardware_dma
    hardware_pwm
    hardware_flash
    hardware_rtc
    tinyusb_board
    tinyusb_host
)

pico_generate_pio_header(rp6502_ria
    ${CMAKE_CURRENT_LIST_DIR}/ria/sys/sys.pio
)

pico_generate_pio_header(rp6502_ria
    ${CMAKE_CURRENT_LIST_DIR}/ria/sys/vga.pio
)

target_sources(rp6502_ria PRIVATE
    ria/main.c
    ria/str.c
    ria/api/api.c
    ria/api/oem.c
    ria/api/rng.c
    ria/api/rtc.c
    ria/api/std.c
    ria/aud/aud.c
    ria/mon/fil.c
    ria/mon/hlp.c
    ria/mon/mon.c
    ria/mon/ram.c
    ria/mon/rom.c
    ria/mon/set.c
    ria/mon/vip.c
    ria/sys/cfg.c
    ria/sys/com.c
    ria/sys/cpu.c
    ria/sys/led.c
    ria/sys/lfs.c
    ria/sys/pix.c
    ria/sys/ria.c
    ria/sys/sys.c
    ria/sys/vga.c
    ria/usb/hid_ps4.c
    ria/usb/hid.c
    ria/usb/msc.c
    ria/usb/usb.c
    fatfs/ff.c
    fatfs/ffunicode.c
    littlefs/lfs.c
    littlefs/lfs_util.c
)


# The Pi Pico VGA

add_executable(rp6502_vga)
pico_add_extra_outputs(rp6502_vga)
pico_set_program_name(rp6502_vga "RP6502 VGA")
pico_set_binary_type(rp6502_vga copy_to_ram)

target_include_directories(rp6502_vga PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/tinyusb/src
    vga
)

target_sources(rp6502_vga PRIVATE
    vga/main.c
    vga/sys/led.c
    vga/sys/pix.c
    vga/sys/ria.c
    vga/sys/std.c
    vga/sys/vga.c
    vga/sys/xram.c
    vga/term/font.c
    vga/term/term.c
    vga/usb/cdc.c
    vga/usb/descriptors.c
    vga/usb/probe.c
    vga/usb/serno.c
)

target_link_libraries(rp6502_vga PRIVATE
    pico_stdlib
    pico_multicore
    pico_unique_id
    pico_scanvideo_dpi
    tinyusb_vga
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/vga/usb/probe.pio
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/vga/sys/pix.pio
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/vga/sys/ria.pio
)

target_compile_definitions(rp6502_vga PRIVATE
    PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=323
    PICO_SCANVIDEO_COLOR_PIN_BASE=6
    PICO_SCANVIDEO_SYNC_PIN_BASE=26
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
)


# Project defines available to both Pi Picos.
# Please change name for hardware forks.
# For release, set version string and set code page to 0.

set_property(TARGET rp6502_ria rp6502_vga
    APPEND PROPERTY COMPILE_DEFINITIONS
    RP6502_NAME="Picocomputer 6502"
    RP6502_VERSION=""
    RP6502_CODE_PAGE=850
    RP6502_MIN_PHI2=500
    RP6502_MAX_PHI2=8000
    RP6502_EXFAT=0
)


# If no version set, touch the file that provides a dev timestamp for the version.

get_target_property(RP6502_RIA_COMPILE_DEFINITIONS rp6502_ria COMPILE_DEFINITIONS)
if(RP6502_RIA_COMPILE_DEFINITIONS MATCHES "RP6502_VERSION=\"\"")
    add_custom_command(TARGET rp6502_ria
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_LIST_DIR}/ria/sys/sys.c
    )
endif()

get_target_property(RP6502_VGA_COMPILE_DEFINITIONS rp6502_vga COMPILE_DEFINITIONS)
if(RP6502_VGA_COMPILE_DEFINITIONS MATCHES "RP6502_VERSION=\"\"")
    add_custom_command(TARGET rp6502_vga
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_LIST_DIR}/vga/sys/ria.c
    )
endif()
