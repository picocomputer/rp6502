# The RP6502-RIA (and W)

# The firmware's own sources; this file is the parts list, they are the parts.
set(RIA_SRC ${RP6502_ROOT}/src/host/pico/ria)

# If you have thoroughly tested a third-party RP2350 board
# in the reference design then please submit a pull request.
if (PICO_BOARD STREQUAL "pico2_w" OR
    PICO_BOARD STREQUAL "pimoroni_pico_plus2_w_rp2350")
    set(RP6502_RIA_W TRUE)
    set(RIA_TARGET rp6502-ria-w)
elseif(PICO_BOARD STREQUAL "pico2")
    set(RP6502_RIA_W FALSE)
    set(RIA_TARGET rp6502-ria)
else()
    message(FATAL_ERROR "Unknown board: ${PICO_BOARD}")
endif()


# The OEM code page tables, lifted out of vendor/fatfs/ffunicode.c by a
# generator so the logic in api/uni.c can be read without a
# preprocessor. See src/core/gen/oem_table_gen.py.
set(OEMCP_GEN ${RP6502_ROOT}/src/core/gen/oem_table_gen.py)
set(OEMCP_SRC ${RP6502_ROOT}/vendor/fatfs/ffunicode.c)
set(OEMCP_C ${CMAKE_CURRENT_BINARY_DIR}/oemcp.c)
set(OEMCP_H ${CMAKE_CURRENT_BINARY_DIR}/oemcp.h)
add_custom_command(OUTPUT ${OEMCP_C} ${OEMCP_H}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${OEMCP_GEN}
        --ffunicode ${OEMCP_SRC} --emit-c ${OEMCP_C} --emit-h ${OEMCP_H}
    DEPENDS ${OEMCP_GEN} ${OEMCP_SRC}
    COMMENT "Generating the OEM code page tables"
    VERBATIM)

# The keyboard layouts, out of def/kbd_*.def and into one image hid/kbl.c
# reads a word at a time. See src/core/gen/kbd_layout_gen.py.
set(KBDLAY_GEN ${RP6502_ROOT}/src/core/gen/kbd_layout_gen.py)
set(KBDLAY_MANIFEST ${RP6502_ROOT}/src/core/def/kbd.def)
file(GLOB KBDLAY_DEFS ${RP6502_ROOT}/src/core/def/kbd_*.def)
set(KBDLAY_C ${CMAKE_CURRENT_BINARY_DIR}/kbdlay.c)
set(KBDLAY_H ${CMAKE_CURRENT_BINARY_DIR}/kbdlay.h)
add_custom_command(OUTPUT ${KBDLAY_C} ${KBDLAY_H}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${KBDLAY_GEN}
        --manifest ${KBDLAY_MANIFEST} --emit-c ${KBDLAY_C} --emit-h ${KBDLAY_H}
    DEPENDS ${KBDLAY_GEN} ${KBDLAY_MANIFEST} ${KBDLAY_DEFS}
    COMMENT "Generating the keyboard layouts"
    VERBATIM)

# core/hid/kbd.c reads core/hid/usage.h, the specification's numbers, on
# every machine. This one also has USB, whose drivers speak TinyUSB's
# spelling of the same specification, and two spellings drift. The check
# lives here because this is the tree that has TinyUSB: the machines that
# need usage.h do not, which is the whole reason it exists.
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
    $<$<COMPILE_LANGUAGE:C>:-Woverride-init>
    $<$<AND:$<CONFIG:Debug>,$<BOOL:${RP6502_RIA_W}>>:-Os>
    $<$<CONFIG:Release>:-Os>
)

# A duplicated locale id in def/str_*.def would silently leave a hole
set_source_files_properties(${RP6502_ROOT}/src/core/str/str.c PROPERTIES COMPILE_OPTIONS "-Werror=override-init")

target_include_directories(${RIA_TARGET} PRIVATE
    ${RP6502_ROOT}/src/host/pico  # host.h, and this host's own
    ${CMAKE_CURRENT_BINARY_DIR}
    ${RIA_SRC}
    ${RP6502_ROOT}/src
    ${RP6502_ROOT}/vendor
)

target_compile_definitions(${RIA_TARGET} PRIVATE
    CYW43_CONFIG_FILE="${RIA_SRC}/cyw43_config.h"
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    LFS_NO_ASSERT=1
    LFS_NO_MALLOC=1
    LFS_NAME_MAX=16
    USE_EMU8950_OPL=1
)

target_sources(${RIA_TARGET} PRIVATE
    ${RIA_SRC}/main.c
    ${RP6502_ROOT}/src/host/pico/host.c
    ${RP6502_ROOT}/src/host/pico/host_ria.c
    ${RP6502_ROOT}/src/core/main.c
    ${RP6502_ROOT}/src/core/api/api.c
    ${RP6502_ROOT}/src/core/api/pro.c
    ${RP6502_ROOT}/src/core/api/arg.c
    ${RP6502_ROOT}/src/core/api/atr.c
    ${RP6502_ROOT}/src/core/api/clk.c
    ${RP6502_ROOT}/src/core/api/dir.c
    ${RP6502_ROOT}/src/core/api/ops.c
    ${RP6502_ROOT}/src/core/sys/main_xreg_0.c
    ${RP6502_ROOT}/src/core/sys/rom_rec.c
    ${RP6502_ROOT}/src/core/sys/rom_win.c
    ${RIA_SRC}/api/fat.c
    ${RP6502_ROOT}/src/core/api/oem.c
    ${RIA_SRC}/api/pro.c
    ${RP6502_ROOT}/src/core/api/uni.c
    ${OEMCP_C}
    ${RP6502_ROOT}/src/core/api/std.c
    ${RIA_SRC}/api/tim.c
    ${RIA_SRC}/aud/aud.c
    ${RP6502_ROOT}/src/core/aud/aud.c
    ${RP6502_ROOT}/src/core/aud/bel.c
    ${RP6502_ROOT}/src/core/aud/bel_presets.c
    ${RP6502_ROOT}/src/core/aud/opl.c
    ${RP6502_ROOT}/src/core/aud/psg.c
    ${RIA_SRC}/ble/ble.c
    ${RIA_SRC}/ble/tlv.c
    ${RP6502_ROOT}/src/core/hid/vt.c
    ${RP6502_ROOT}/src/core/hid/hid.c
    ${RP6502_ROOT}/src/core/hid/kbd.c
    ${RP6502_ROOT}/src/core/hid/kbl.c
    ${RP6502_ROOT}/src/core/hid/kbt.c
    ${KBDLAY_C}
    ${RP6502_ROOT}/src/core/hid/mou.c
    ${RP6502_ROOT}/src/core/hid/pad.c
    ${RP6502_ROOT}/src/core/hid/parse.c
    ${RP6502_ROOT}/src/core/hid/tab.c
    ${RIA_SRC}/mon/dsk.c
    ${RIA_SRC}/mon/fil.c
    ${RIA_SRC}/mon/help.c
    ${RIA_SRC}/mon/mon.c
    ${RIA_SRC}/mon/ram.c
    ${RIA_SRC}/mon/rom.c
    ${RIA_SRC}/mon/set.c
    ${RIA_SRC}/mon/uf2.c
    ${RIA_SRC}/mon/vip.c
    ${RIA_SRC}/net/cmd.c
    ${RIA_SRC}/net/cyw.c
    ${RIA_SRC}/net/mdm.c
    ${RIA_SRC}/net/net.c
    ${RIA_SRC}/net/ntp.c
    ${RIA_SRC}/net/telnet.c
    ${RIA_SRC}/net/wfi.c
    ${RP6502_ROOT}/src/core/str/rln.c
    ${RP6502_ROOT}/src/core/str/str.c
    ${RIA_SRC}/sys/cfg.c
    ${RIA_SRC}/sys/com.c
    ${RIA_SRC}/sys/com_telnet.c
    ${RIA_SRC}/sys/com_utf8.c
    ${RIA_SRC}/sys/cpu.c
    ${RIA_SRC}/sys/led.c
    ${RIA_SRC}/sys/lfs.c
    ${RIA_SRC}/sys/mem.c
    ${RIA_SRC}/sys/path.c
    ${RIA_SRC}/sys/pix.c
    ${RIA_SRC}/sys/ria.c
    ${RIA_SRC}/sys/sys.c
    ${RIA_SRC}/sys/vga.c
    ${RIA_SRC}/usb/mid.c
    ${RIA_SRC}/usb/msc.c
    ${RIA_SRC}/usb/nfc.c
    ${RIA_SRC}/usb/usb.c
    ${RIA_SRC}/usb/vcp.c
    ${RIA_SRC}/usb/xin.c
)

rp6502_use_version_header(${RIA_TARGET}
    ${RIA_SRC}/sys/sys.c
)

target_sources(${RIA_TARGET} PRIVATE
    ${RP6502_ROOT}/vendor/emu8950/emu8950.c
    ${RP6502_ROOT}/vendor/fatfs/ff.c
    ${RP6502_ROOT}/vendor/littlefs/lfs.c
    ${RP6502_ROOT}/vendor/littlefs/lfs_util.c
)

# emu8950 is vendored verbatim; silence its host-GCC warnings rather than patch
# upstream (unused args/function, sizeof in calloc's first argument).
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
    pico_btstack_ble
    cmsis_core
)

pico_generate_pio_header(${RIA_TARGET}
    ${RIA_SRC}/ria.pio
)

# Override various lib files with custom code.
# The magic is that header-only files don't get compiled.
set_source_files_properties(
    ${PICO_SDK_PATH}/src/rp2_common/pico_btstack/btstack_flash_bank.c
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
