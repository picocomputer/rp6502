# The RIA firmware, as a parts list. Both boards are made of these; a machine
# beside this file sets RIA_TARGET, includes it, and then adds what its own
# board has. The radio is a board, not a configuration, so nothing here knows
# whether there is one -- see host/pico/ria-w.
#
# RIA_TARGET  the executable to build, named by the machine that includes this.

# The firmware's own sources; this file is the parts list, they are the parts.
set(RIA_SRC ${RP6502_ROOT}/src/host/pico/ria)
# The directory that answers osal/os.h, osal/fs.h and osal/dir.h for this
# machine; see src/host/emu.cmake for the same name on the desktops.
set(RP6502_OSAL ${RP6502_ROOT}/src/osal/pico)

include(${RP6502_ROOT}/src/core/gen.cmake)
rp6502_gen_oemcp(oemcp)
rp6502_gen_kbdlay(kbdlay)

# core/hid/keyboard.c reads core/hid/usage.h, the specification's numbers, on
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

set_target_properties(${RIA_TARGET} PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION TRUE
)

# Avoid 26KB of unicode and jis we don't need, and the second printf engine
# newlib's strftime would otherwise drag in through sniprintf.
target_link_options(${RIA_TARGET} PRIVATE ${IPO_PRINTF_LINK_OPTIONS}
    -Wl,--wrap=iswspace
    -Wl,-u,__wrap_iswspace
    -Wl,--wrap=sniprintf
    -Wl,-u,__wrap_sniprintf
)

target_compile_options(${RIA_TARGET} PRIVATE
    # What this machine says about itself, before anything can take a default.
    # Forced rather than found on a path: it has to reach every unit or none,
    # or a symbol lands in RAM that belongs in flash. C and C++ only -- the
    # SDK's crt0.S goes to the assembler, which cannot read a C header.
    "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${RP6502_ROOT}/src/host/pico/machine.h>"
    -Wall -Wextra -Wsign-compare
    $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>
    $<$<COMPILE_LANGUAGE:C>:-Woverride-init>
    $<$<CONFIG:Release>:-Os>
)

# A duplicated locale id in def/str_*.def would silently leave a hole
set_source_files_properties(${RP6502_ROOT}/src/core/str/str.c PROPERTIES COMPILE_OPTIONS "-Werror=override-init")

target_include_directories(${RIA_TARGET} PRIVATE
    ${RP6502_ROOT}/src/host/pico  # the pico's own headers
    ${CMAKE_CURRENT_SOURCE_DIR} # drivers.h, the machine's own
    ${CMAKE_CURRENT_BINARY_DIR}
    ${RIA_SRC}
    ${RP6502_ROOT}/src
    ${RP6502_ROOT}/vendor
)

target_compile_definitions(${RIA_TARGET} PRIVATE
    # Nothing in this firmware formats a float; sizes render with integer math.
    PICO_PRINTF_SUPPORT_FLOAT=0
    PICO_PRINTF_SUPPORT_EXPONENTIAL=0
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
    ${RP6502_ROOT}/src/core/sys/sys.c
    ${RP6502_ROOT}/src/host/version.c
    ${RP6502_ROOT}/src/core/api/api.c
    ${RP6502_ROOT}/src/core/api/proc.c
    ${RP6502_ROOT}/src/core/api/arg.c
    ${RP6502_ROOT}/src/core/api/attr.c
    ${RP6502_ROOT}/src/core/api/clk.c
    ${RP6502_ROOT}/src/core/api/dir.c
    ${RP6502_ROOT}/src/core/api/ops.c
    ${RP6502_ROOT}/src/core/api/xreg0.c
    ${RP6502_ROOT}/src/core/rom/asset.c
    ${RP6502_ROOT}/src/core/rom/pump.c
    ${RP6502_OSAL}/dir.c
    ${RP6502_OSAL}/errmap.c
    ${RP6502_OSAL}/fs.c
    ${RP6502_OSAL}/lfs.c
    ${RP6502_OSAL}/os.c
    ${RP6502_ROOT}/src/core/str/path.c
    ${RP6502_ROOT}/src/core/str/oem.c
    ${RIA_SRC}/api/proc.c
    ${RP6502_ROOT}/src/core/str/unicode.c
    ${OEMCP_C}
    ${RP6502_ROOT}/src/core/api/std.c
    ${RIA_SRC}/api/tim.c
    ${RIA_SRC}/aud/aud.c
    ${RP6502_ROOT}/src/core/aud/aud.c
    ${RP6502_ROOT}/src/core/aud/bel.c
    ${RP6502_ROOT}/src/core/aud/bel_presets.c
    ${RP6502_ROOT}/src/core/aud/opl.c
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
    ${RIA_SRC}/sys/cfg.c
    ${RIA_SRC}/sys/com.c
    ${RIA_SRC}/sys/com_telnet.c
    ${RIA_SRC}/sys/cpu.c
    ${RIA_SRC}/sys/led.c
    ${RIA_SRC}/sys/mem.c
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
    ${RP6502_ROOT}/src/host/version.c
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
