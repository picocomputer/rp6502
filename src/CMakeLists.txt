# If you have thoroughly tested a third-party RP2350 board
# in the reference design then please submit a pull request.

if (PICO_BOARD STREQUAL "pico2_w" OR
    PICO_BOARD STREQUAL "pimoroni_pico_plus2_w_rp2350")
    set(RP6502_RIA_W TRUE)
    set(RIA_TARGET rp6502_ria_w)
elseif(PICO_BOARD STREQUAL "pico2")
    set(RP6502_RIA_W FALSE)
    set(RIA_TARGET rp6502_ria)
else()
    message(FATAL_ERROR "Unknown board: ${PICO_BOARD}")
endif()


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


# The RP6502-RIA (and W)

add_executable(${RIA_TARGET})
pico_add_extra_outputs(${RIA_TARGET})
pico_set_binary_type(${RIA_TARGET} copy_to_ram)

if (RP6502_RIA_W)
    target_compile_definitions(${RIA_TARGET} PRIVATE RP6502_RIA_W=1)
    target_link_libraries(${RIA_TARGET} PRIVATE pico_cyw43_arch_lwip_poll pico_btstack_cyw43)
    pico_set_program_name(${RIA_TARGET} "RP6502-RIA-W")
else()
    pico_set_program_name(${RIA_TARGET} "RP6502-RIA")
endif()

set_target_properties(${RIA_TARGET} PROPERTIES 
    INTERPROCEDURAL_OPTIMIZATION TRUE
)

target_link_options(${RIA_TARGET} PRIVATE ${IPO_PRINTF_LINK_OPTIONS})

target_compile_options(${RIA_TARGET} PRIVATE
    -Wall -Wextra
    $<$<AND:$<CONFIG:Debug>,$<BOOL:${RP6502_RIA_W}>>:-Os>
    $<$<CONFIG:Release>:-Os>
)

target_include_directories(${RIA_TARGET} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/tinyusb/src
    ria
)

target_compile_definitions(${RIA_TARGET} PRIVATE
    CYW43_CONFIG_FILE="${CMAKE_CURRENT_LIST_DIR}/ria/cyw43_config.h"
    pico_flash_bank_get_storage_offset_func=lfs_get_bt_storage_offset
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    LFS_NO_ASSERT=1
    LFS_NO_MALLOC=1
    LFS_NAME_MAX=16
)

target_link_libraries(${RIA_TARGET} PRIVATE
    pico_aon_timer
    pico_stdlib
    pico_multicore
    pico_rand
    hardware_pio
    hardware_dma
    hardware_pwm
    hardware_flash
    tinyusb_host
    pico_btstack_ble
)

pico_generate_pio_header(${RIA_TARGET}
    ${CMAKE_CURRENT_LIST_DIR}/ria/ria.pio
)

target_sources(${RIA_TARGET} PRIVATE
    fatfs/ff.c
    fatfs/ffunicode.c
    littlefs/lfs.c
    littlefs/lfs_util.c
    ria/main.c
    ria/api/api.c
    ria/api/clk.c
    ria/api/dir.c
    ria/api/oem.c
    ria/api/rng.c
    ria/api/std.c
    ria/aud/aud.c
    ria/aud/psg.c
    ria/hid/hid.c
    ria/hid/kbd.c
    ria/hid/mou.c
    ria/hid/pad.c
    ria/mon/fil.c
    ria/mon/hlp.c
    ria/mon/mon.c
    ria/mon/ram.c
    ria/mon/rom.c
    ria/mon/set.c
    ria/mon/str.c
    ria/mon/vip.c
    ria/net/ble.c
    ria/net/cmd.c
    ria/net/cyw.c
    ria/net/mdm.c
    ria/net/ntp.c
    ria/net/tel.c
    ria/net/wfi.c
    ria/sys/cfg.c
    ria/sys/com.c
    ria/sys/cpu.c
    ria/sys/led.c
    ria/sys/lfs.c
    ria/sys/mem.c
    ria/sys/pix.c
    ria/sys/ria.c
    ria/sys/rln.c
    ria/sys/sys.c
    ria/sys/vga.c
    ria/usb/msc.c
    ria/usb/usb.c
    ria/usb/xin.c
)


# The RP6502-VGA

add_executable(rp6502_vga)
pico_add_extra_outputs(rp6502_vga)
pico_set_program_name(rp6502_vga "RP6502 VGA")
pico_set_binary_type(rp6502_vga copy_to_ram)

set_target_properties(rp6502_vga PROPERTIES 
    INTERPROCEDURAL_OPTIMIZATION TRUE
)

target_link_options(rp6502_vga PRIVATE ${IPO_PRINTF_LINK_OPTIONS})

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
    vga/scanvideo/scanvideo.c
    vga/sys/com.c
    vga/sys/led.c
    vga/sys/mem.c
    vga/sys/pix.c
    vga/sys/ria.c
    vga/sys/sys.c
    vga/sys/vga.c
    vga/term/color.c
    vga/term/font.c
    vga/term/term.c
    vga/usb/cdc.c
    vga/usb/descriptors.c
    vga/usb/usb.c
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
    ${CMAKE_CURRENT_LIST_DIR}/vga/vga.pio
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/vga/scanvideo/scanvideo.pio
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/vga/scanvideo/timing.pio
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

set_property(TARGET ${RIA_TARGET} rp6502_vga
    APPEND PROPERTY COMPILE_DEFINITIONS
    RP6502_NAME="Picocomputer 6502"
    RP6502_VERSION=""
    RP6502_CODE_PAGE=437
    RP6502_EXFAT=0
)


# If no version set, set a define for C preprocessor and touch
# the file that provides a dev timestamp for the version.

get_target_property(RP6502_RIA_COMPILE_DEFINITIONS ${RIA_TARGET} COMPILE_DEFINITIONS)
if(RP6502_RIA_COMPILE_DEFINITIONS MATCHES "RP6502_VERSION=\"\"")
    set_property(TARGET ${RIA_TARGET}
        APPEND PROPERTY COMPILE_DEFINITIONS
        RP6502_VERSION_EMPTY=1
    )
    add_custom_command(TARGET ${RIA_TARGET} PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_LIST_DIR}/ria/sys/sys.c
    )
endif()

get_target_property(RP6502_VGA_COMPILE_DEFINITIONS rp6502_vga COMPILE_DEFINITIONS)
if(RP6502_VGA_COMPILE_DEFINITIONS MATCHES "RP6502_VERSION=\"\"")
    set_property(TARGET rp6502_vga
        APPEND PROPERTY COMPILE_DEFINITIONS
        RP6502_VERSION_EMPTY=1
    )
    add_custom_command(TARGET rp6502_vga PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_LIST_DIR}/vga/sys/sys.c
    )
endif()
