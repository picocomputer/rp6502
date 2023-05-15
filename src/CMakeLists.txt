# Bleeding edge tinyusb needed for mass storage devices

add_library(tinyusb_vga STATIC EXCLUDE_FROM_ALL)

target_include_directories(tinyusb_vga PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/tinyusb/src
    probe
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
    dev
)

target_compile_definitions(rp6502_ria PRIVATE
    RP6502_NAME="Picocomputer 6502"
    RP6502_CODE_PAGE=437
    RP6502_EXFAT=0
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
    tinyusb_board
    tinyusb_host
)

pico_generate_pio_header(rp6502_ria
    ${CMAKE_CURRENT_LIST_DIR}/ria/ria.pio
)

target_sources(rp6502_ria PRIVATE
    dev/aud.c
    dev/com.c
    dev/dev.c
    dev/hid_ps4.c
    dev/hid.c
    dev/lfs.c
    dev/msc.c
    mem/mbuf.c
    mem/xram.c
    mem/xstack.c
    mon/fil.c
    mon/hlp.c
    mon/mon.c
    mon/rom.c
    mon/set.c
    mon/str.c
    mon/sys.c
    mon/vip.c
    ria/act.c
    ria/api.c
    ria/cfg.c
    ria/main.c
    ria/ria.c
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
    probe
)

target_sources(rp6502_vga PRIVATE
    mem/xram.c
    vga/main.c
    vga/pix.c
    vga/term.c
    vga/vga.c
    probe/led.c
    probe/probe.c
    probe/cdc_uart.c
    probe/get_serial.c
    probe/usb_descriptors.c
)

target_link_libraries(rp6502_vga PRIVATE
    pico_stdlib
    pico_multicore
    pico_unique_id
    pico_scanvideo_dpi
    tinyusb_vga
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/probe/probe.pio
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/vga/pix.pio
)

target_compile_definitions(rp6502_vga PRIVATE
    PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=323
    PICO_SCANVIDEO_COLOR_PIN_BASE=6
    PICO_SCANVIDEO_SYNC_PIN_BASE=26
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
)
