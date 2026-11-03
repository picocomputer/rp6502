set(RIA_SRC ${RP6502_ROOT}/src/host/pico/ria)
include(${RP6502_ROOT}/src/osal/pico/pico.cmake)

include(${RP6502_ROOT}/src/core/gen.cmake)
rp6502_gen_oemcp(oemcp)
rp6502_gen_kbdlay(kbdlay)

set(HID_USAGE_STAMP ${CMAKE_CURRENT_BINARY_DIR}/hid_usage.stamp)
set(HID_USAGE ${RP6502_ROOT}/src/core/hid/usage.h)
set(HID_VENDOR ${RP6502_ROOT}/vendor/tinyusb/src/class/hid/hid.h)
add_custom_command(OUTPUT ${HID_USAGE_STAMP}
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_ROOT}/src/core/gen/hid_usage_check.py
        --usage ${HID_USAGE} --vendor ${HID_VENDOR}
    COMMAND ${CMAKE_COMMAND} -E touch ${HID_USAGE_STAMP}
    DEPENDS ${RP6502_ROOT}/src/core/gen/hid_usage_check.py ${HID_USAGE} ${HID_VENDOR}
    COMMENT "Checking core/hid/usage.h against TinyUSB"
    VERBATIM)
add_custom_target(hid_usage DEPENDS ${HID_USAGE_STAMP})

add_executable(${RIA_TARGET})
add_dependencies(${RIA_TARGET} hid_usage)
rp6502_osal_pico(${RIA_TARGET})
target_compile_definitions(${RIA_TARGET} PRIVATE ${RP6502_PROJECT_DEFINITIONS})
rp6502_log_definitions(${RIA_TARGET} PRIVATE)
pico_add_extra_outputs(${RIA_TARGET})
pico_set_binary_type(${RIA_TARGET} copy_to_ram)

set_target_properties(${RIA_TARGET} PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION TRUE
)

# Avoid 26KB of unicode and jis we don't need. Newlib's strftime calls
# sniprintf, which is wrapped to call vsnprintf so that newlib's printf
# engine is not linked beside pico_printf.
target_link_options(${RIA_TARGET} PRIVATE ${IPO_PRINTF_LINK_OPTIONS}
    -Wl,--wrap=iswspace
    -Wl,-u,__wrap_iswspace
    -Wl,--wrap=sniprintf
    -Wl,-u,__wrap_sniprintf
)

target_compile_options(${RIA_TARGET} PRIVATE
    -Wall -Wextra -Wsign-compare
    $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>
    $<$<COMPILE_LANGUAGE:C>:-Woverride-init>
    $<$<CONFIG:Release>:-Os>
)

set_source_files_properties(${RP6502_ROOT}/src/core/str/str.c PROPERTIES COMPILE_OPTIONS "-Werror=override-init")

# __aeabi_read_tp's naked inline-asm literal pool loads can end up out of
# the assembler's encodable range once whole-program LTO merges everything
# into one giant unit. Keep this one file out of LTO.
# **! Obsolete after @rumbledethumps' PICO_THREAD_LOCAL_TYPE Commit 20f16c0 **
# set_source_files_properties(${PICO_SDK_PATH}/src/rp2_common/pico_thread_local/thread_local.c
#    PROPERTIES COMPILE_OPTIONS "-fno-lto")

target_include_directories(${RIA_TARGET} PRIVATE
    ${RP6502_ROOT}/src/host/pico
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${RIA_SRC}
    ${RP6502_ROOT}/src
    ${RP6502_ROOT}/vendor
)

target_compile_definitions(${RIA_TARGET} PRIVATE
    PICO_PRINTF_SUPPORT_FLOAT=0
    PICO_PRINTF_SUPPORT_EXPONENTIAL=0
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    LFS_NO_ASSERT=1
    LFS_NO_MALLOC=1
    LFS_NAME_MAX=16
    LFS_DEFINES=osal/pico/lfs_log.h
    USE_EMU8950_OPL=1
)

target_sources(${RIA_TARGET} PRIVATE
    ${RIA_SRC}/main.c
    ${RP6502_ROOT}/src/host/pico/host.c
    ${RP6502_ROOT}/src/core/sys/sys.c
    ${RP6502_ROOT}/src/core/sys/version.c
    ${RP6502_ROOT}/src/core/api/api.c
    ${RP6502_ROOT}/src/core/api/proc.c
    ${RP6502_ROOT}/src/core/api/arg.c
    ${RP6502_ROOT}/src/core/api/attr.c
    ${RP6502_ROOT}/src/core/api/clk.c
    ${RP6502_ROOT}/src/core/api/dir.c
    ${RP6502_ROOT}/src/core/api/ops.c
    ${RP6502_ROOT}/src/core/api/save.c
    ${RP6502_ROOT}/src/core/api/xreg0.c
    ${RP6502_ROOT}/src/core/rom/alias.c
    ${RP6502_ROOT}/src/core/rom/asset.c
    ${RP6502_ROOT}/src/core/rom/pump.c
    ${RP6502_ROOT}/src/core/str/path.c
    ${RP6502_ROOT}/src/core/str/oem.c
    ${RIA_SRC}/api/proc.c
    ${RP6502_ROOT}/src/core/str/unicode.c
    ${OEMCP_C}
    ${RP6502_ROOT}/src/core/api/std.c
    ${RIA_SRC}/api/tim.c
    ${RIA_SRC}/aud/mix.c
    ${RP6502_ROOT}/src/core/aud/sine.c
    ${RP6502_ROOT}/src/core/aud/bel.c
    ${RP6502_ROOT}/src/core/aud/bel_presets.c
    ${RP6502_ROOT}/src/core/aud/opl.c
    ${RP6502_ROOT}/src/core/aud/pcm.c
    ${RP6502_ROOT}/src/core/aud/psg.c
    ${RP6502_ROOT}/src/core/hid/hid.c
    ${RP6502_ROOT}/src/core/hid/keyboard.c
    ${RP6502_ROOT}/src/core/hid/layout.c
    ${RP6502_ROOT}/src/core/hid/keymap.c
    ${KBDLAY_C}
    ${RP6502_ROOT}/src/core/hid/mouse.c
    ${RP6502_ROOT}/src/core/hid/gamepad.c
    ${RP6502_ROOT}/src/core/hid/parse.c
    ${RP6502_ROOT}/src/core/hid/tablet.c
    ${RIA_SRC}/mon/drive.c
    ${RIA_SRC}/mon/fil.c
    ${RIA_SRC}/mon/help.c
    ${RIA_SRC}/mon/mon.c
    ${RIA_SRC}/mon/ram.c
    ${RIA_SRC}/mon/rom.c
    ${RIA_SRC}/mon/set.c
    ${RIA_SRC}/mon/status.c
    ${RIA_SRC}/mon/uf2.c
    ${RP6502_ROOT}/src/core/str/rln.c
    ${RP6502_ROOT}/src/core/str/str.c
    ${RP6502_ROOT}/src/core/sys/config.c
    ${RP6502_ROOT}/src/core/sys/random.c
    ${RP6502_ROOT}/src/core/sys/timer.c
    ${RP6502_ROOT}/src/core/sys/xram.c
    ${RP6502_ROOT}/src/core/ria/regs.c
    ${RIA_SRC}/sys/cfg.c
    ${RIA_SRC}/sys/com.c
    ${RIA_SRC}/sys/com_telnet.c
    ${RP6502_ROOT}/src/core/com/pick.c
    ${RIA_SRC}/sys/phi2.c
    ${RIA_SRC}/sys/rp2350.c
    ${RIA_SRC}/sys/resb.c
    ${RIA_SRC}/sys/led.c
    ${RIA_SRC}/sys/mbuf.c
    ${RIA_SRC}/sys/path.c
    ${RIA_SRC}/sys/pix.c
    ${RIA_SRC}/sys/ria.c
    ${RIA_SRC}/sys/vga.c
    ${RIA_SRC}/usb/mid.c
    ${RIA_SRC}/usb/msc.c
    ${RIA_SRC}/usb/nfc.c
    ${RIA_SRC}/usb/usb.c
    ${RIA_SRC}/usb/vcp.c
    ${RIA_SRC}/usb/xin.c
)

rp6502_use_version_header(${RIA_TARGET}
    ${RP6502_ROOT}/src/core/sys/version.c
)

target_sources(${RIA_TARGET} PRIVATE
    ${RP6502_ROOT}/vendor/emu8950/emu8950.c
    ${RP6502_ROOT}/vendor/fatfs/ff.c
    ${RP6502_ROOT}/vendor/littlefs/lfs.c
    ${RP6502_ROOT}/vendor/littlefs/lfs_util.c
)

set_source_files_properties(
    ${RP6502_ROOT}/vendor/emu8950/emu8950.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-unused-function;-Wno-calloc-transposed-args"
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
    boot_uf2_headers
    tinyusb_host
    cmsis_core
)

pico_generate_pio_header(${RIA_TARGET}
    ${RIA_SRC}/ria.pio
)

# Override various lib files with custom code.
# The magic is that header-only files don't get compiled.
set_source_files_properties(
    ${RP6502_ROOT}/vendor/tinyusb/src/portable/raspberrypi/rp2040/hcd_rp2040.c
    ${RP6502_ROOT}/vendor/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c
    ${RP6502_ROOT}/vendor/tinyusb/src/class/midi/midi_host.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)
target_sources(${RIA_TARGET} PRIVATE
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/hcd_rp2040.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/rp2040_usb.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/midi_host.c
)
