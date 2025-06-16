# The Pi Pico RIA

add_executable(rp6502_ria)
pico_add_extra_outputs(rp6502_ria)
pico_set_binary_type(rp6502_ria copy_to_ram)

if (PICO_BOARD STREQUAL "pico2_w" OR
    PICO_BOARD STREQUAL "pimoroni_pico_plus2_w_rp2350")
    target_compile_definitions(rp6502_ria PRIVATE RP6502_RIA_W)
    target_link_libraries(rp6502_ria PRIVATE pico_cyw43_arch_lwip_poll)
    pico_set_program_name(rp6502_ria "RP6502 RIA W")
else()
    pico_set_program_name(rp6502_ria "RP6502 RIA")
endif()

target_include_directories(rp6502_ria PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/tinyusb/src
    ria
)

target_compile_definitions(rp6502_ria PRIVATE
    CYW43_CONFIG_FILE="${CMAKE_CURRENT_LIST_DIR}/ria/cyw43_config.h"
    pico_flash_bank_get_storage_offset_func=lfs_get_bt_storage_offset
    LFS_NO_ASSERT=1
    LFS_NO_MALLOC=1
    LFS_NAME_MAX=16
)

target_compile_options(rp6502_ria PRIVATE
    -Wall -Wextra
)

target_link_libraries(rp6502_ria PRIVATE
    pico_aon_timer
    pico_stdlib
    pico_multicore
    pico_rand
    hardware_pio
    hardware_dma
    hardware_pwm
    hardware_flash
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
    ria/api/clk.c
    ria/api/oem.c
    ria/api/rng.c
    ria/api/std.c
    ria/aud/aud.c
    ria/aud/psg.c
    ria/mon/fil.c
    ria/mon/hlp.c
    ria/mon/mon.c
    ria/mon/ram.c
    ria/mon/rom.c
    ria/mon/set.c
    ria/mon/vip.c
    ria/net/cyw.c
    ria/net/mdm.c
    ria/net/ntp.c
    ria/net/nvr.c
    ria/net/wfi.c
    ria/sys/cfg.c
    ria/sys/com.c
    ria/sys/cpu.c
    ria/sys/led.c
    ria/sys/lfs.c
    ria/sys/mem.c
    ria/sys/pix.c
    ria/sys/ria.c
    ria/sys/sys.c
    ria/sys/vga.c
    ria/usb/hid.c
    ria/usb/kbd.c
    ria/usb/mou.c
    ria/usb/msc.c
    ria/usb/pad.c
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

target_compile_options(rp6502_vga PRIVATE
    -Wall -Wextra
)

target_include_directories(rp6502_vga PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/tinyusb/src
    vga
)

target_sources(rp6502_vga PRIVATE
    vga/main.c
    vga/modes/mode1.c
    vga/modes/mode2.c
    vga/modes/mode3.c
    vga/modes/mode4.c
    vga/modes/mode4.S
    vga/sys/led.c
    vga/sys/mem.c
    vga/sys/pix.c
    vga/sys/ria.c
    vga/sys/std.c
    vga/sys/vga.c
    vga/term/color.c
    vga/term/font.c
    vga/term/term.c
    vga/usb/cdc.c
    vga/usb/descriptors.c
    vga/usb/serno.c
    scanvideo/scanvideo.c
)

target_link_libraries(rp6502_vga PRIVATE
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_pio
    hardware_dma
    hardware_interp
    tinyusb_device
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/vga/sys/pix.pio
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/vga/sys/ria.pio
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/scanvideo/scanvideo.pio
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/scanvideo/timing.pio
)

target_compile_definitions(rp6502_vga PRIVATE
    PICO_SCANVIDEO_PLANE_COUNT=3
    PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=323
    PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT=10
    PICO_SCANVIDEO_COLOR_PIN_BASE=6
    PICO_SCANVIDEO_SYNC_PIN_BASE=26
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
    PICO_RP2040_USB_DEVICE_UFRAME_FIX=1
)


# Project defines available to both Pi Picos.
# Please change name for hardware forks.
# For release, set version string and set code page to 0.

set_property(TARGET rp6502_ria rp6502_vga
    APPEND PROPERTY COMPILE_DEFINITIONS
    RP6502_NAME="Picocomputer 6502"
    RP6502_VERSION=""
    RP6502_CODE_PAGE=850
    RP6502_KEYBOARD=EN_US
    RP6502_MIN_PHI2=750
    RP6502_MAX_PHI2=8000
    RP6502_EXFAT=0
)


# If no version set, touch the file that provides a dev timestamp for the version.

get_target_property(RP6502_RIA_COMPILE_DEFINITIONS rp6502_ria COMPILE_DEFINITIONS)
if(RP6502_RIA_COMPILE_DEFINITIONS MATCHES "RP6502_VERSION=\"\"")
    add_custom_command(TARGET rp6502_ria PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_LIST_DIR}/ria/sys/sys.c
    )
endif()

get_target_property(RP6502_VGA_COMPILE_DEFINITIONS rp6502_vga COMPILE_DEFINITIONS)
if(RP6502_VGA_COMPILE_DEFINITIONS MATCHES "RP6502_VERSION=\"\"")
    add_custom_command(TARGET rp6502_vga PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_LIST_DIR}/vga/sys/ria.c
    )
endif()
