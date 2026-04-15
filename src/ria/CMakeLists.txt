# The RP6502-RIA (and W)

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

add_executable(${RIA_TARGET})
target_compile_definitions(${RIA_TARGET} PRIVATE ${RP6502_PROJECT_DEFINITIONS})
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

# Avoid 26KB of unicode and jis we don't need
target_link_options(${RIA_TARGET} PRIVATE ${IPO_PRINTF_LINK_OPTIONS}
    -Wl,--wrap=iswspace 
    -Wl,-u,__wrap_iswspace
)

target_compile_options(${RIA_TARGET} PRIVATE
    -Wall -Wextra -Wsign-compare
    $<$<AND:$<CONFIG:Debug>,$<BOOL:${RP6502_RIA_W}>>:-Os>
    $<$<CONFIG:Release>:-Os>
)

target_include_directories(${RIA_TARGET} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${PROJECT_SOURCE_DIR}/src
)

target_compile_definitions(${RIA_TARGET} PRIVATE
    CYW43_CONFIG_FILE="${CMAKE_CURRENT_LIST_DIR}/cyw43_config.h"
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    LFS_NO_ASSERT=1
    LFS_NO_MALLOC=1
    LFS_NAME_MAX=16
    USE_EMU8950_OPL=1
)

target_sources(${RIA_TARGET} PRIVATE
    main.c
    api/api.c
    api/atr.c
    api/clk.c
    api/dir.c
    api/oem.c
    api/pro.c
    api/std.c
    aud/aud.c
    aud/bel.c
    aud/opl.c
    aud/psg.c
    hid/hid.c
    hid/kbd.c
    hid/mou.c
    hid/pad.c
    mon/fil.c
    mon/hlp.c
    mon/mon.c
    mon/ram.c
    mon/rom.c
    mon/set.c
    mon/vip.c
    net/ble.c
    net/cmd.c
    net/cyw.c
    net/mdm.c
    net/net.c
    net/ntp.c
    net/tel.c
    net/wfi.c
    str/rln.c
    str/str.c
    sys/cfg.c
    sys/com.c
    sys/cpu.c
    sys/led.c
    sys/lfs.c
    sys/mem.c
    sys/pix.c
    sys/ria.c
    sys/sys.c
    sys/vga.c
    usb/msc.c
    usb/nfc.c
    usb/usb.c
    usb/vcp.c
    usb/xin.c
)

target_sources(${RIA_TARGET} PRIVATE
    ${PROJECT_SOURCE_DIR}/src/emu8950/emu8950.c
    ${PROJECT_SOURCE_DIR}/src/fatfs/ff.c
    ${PROJECT_SOURCE_DIR}/src/fatfs/ffunicode.c
    ${PROJECT_SOURCE_DIR}/src/littlefs/lfs.c
    ${PROJECT_SOURCE_DIR}/src/littlefs/lfs_util.c
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
    cmsis_core
)

pico_generate_pio_header(${RIA_TARGET}
    ${CMAKE_CURRENT_LIST_DIR}/ria.pio
)

# Override various lib files with custom code.
# The magic is that header-only files don't get compiled.
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/src/tinyusb/src/portable/raspberrypi/rp2040/hcd_rp2040.c
    ${PROJECT_SOURCE_DIR}/src/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c
    ${PROJECT_SOURCE_DIR}/src/tinyusb/src/class/msc/msc_host.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_btstack/btstack_flash_bank.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)
target_sources(${RIA_TARGET} PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tinyusb_rp6502/hcd_rp2040.c
    ${PROJECT_SOURCE_DIR}/src/tinyusb_rp6502/rp2040_usb.c
    ${PROJECT_SOURCE_DIR}/src/tinyusb_rp6502/msc_host.c
    net/ble_tlv.c
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
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_LIST_DIR}/sys/sys.c
    )
endif()
