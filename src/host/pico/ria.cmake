set(RIA_SRC ${RP6502_ROOT}/src/host/pico/ria)
include(${RP6502_ROOT}/src/osal/pico/pico.cmake)

include(${RP6502_ROOT}/src/core/gen.cmake)
rp6502_gen_c()

set(HID_USAGE_STAMP ${CMAKE_CURRENT_BINARY_DIR}/hid_usage.stamp)
set(HID_USAGE ${RP6502_ROOT}/src/core/hid/usage.h)
set(HID_VENDOR ${RP6502_ROOT}/vendor/tinyusb/src/class/hid/hid.h)
add_custom_command(OUTPUT ${HID_USAGE_STAMP}
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_ROOT}/src/core/hid/usage.py
        --usage ${HID_USAGE} --vendor ${HID_VENDOR}
    COMMAND ${CMAKE_COMMAND} -E touch ${HID_USAGE_STAMP}
    DEPENDS ${RP6502_ROOT}/src/core/hid/usage.py ${HID_USAGE} ${HID_VENDOR}
    COMMENT "Checking core/hid/usage.h against TinyUSB"
    VERBATIM)
add_custom_target(hid_usage DEPENDS ${HID_USAGE_STAMP})

add_executable(${RIA_TARGET})
add_dependencies(${RIA_TARGET} hid_usage rp6502_gen_c)
rp6502_osal_pico(${RIA_TARGET})
target_compile_definitions(${RIA_TARGET} PRIVATE ${RP6502_PROJECT_DEFINITIONS})
rp6502_log_definitions(${RIA_TARGET} PRIVATE)
pico_add_extra_outputs(${RIA_TARGET})
pico_set_binary_type(${RIA_TARGET} default)

# Sources that run in an interrupt, on the main loop's hot path, or while the
# RIA rewrites its own flash, whole: the SDK's, TinyUSB's, FatFs, the storage
# path, the consoles, the network and the bell. The linker can keep code out of
# flash only by its object file, so these skip LTO. The SDK's flash .text and
# .rodata take everything but the files its one-line default_text_excludes.incl
# and default_rodata_excludes.incl name, and whatever they skip lands in RAM
# with .data. These overrides add the list.
file(GLOB _ria_lwip_core
    ${PICO_SDK_PATH}/lib/lwip/src/core/*.c
    ${PICO_SDK_PATH}/lib/lwip/src/core/ipv4/*.c)
set(RIA_RAM_SOURCES
    ${PICO_SDK_PATH}/src/common/pico_time/time.c
    ${PICO_SDK_PATH}/src/rp2_common/hardware_gpio/gpio.c
    ${PICO_SDK_PATH}/src/rp2_common/hardware_irq/irq.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_rand/rand.c
    ${PICO_SDK_PATH}/src/rp2_common/hardware_timer/timer.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_async_context/async_context_base.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_async_context/async_context_poll.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_arch/cyw43_arch.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_driver/cyw43_driver.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_driver/cyw43_bus_pio_spi.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_driver/btstack_hci_transport_cyw43.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_lwip/lwip_nosys.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_printf/printf.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_stdio/stdio.c
    ${PICO_SDK_PATH}/src/rp2_common/hardware_watchdog/watchdog.c
    ${PICO_SDK_PATH}/src/rp2_common/hardware_clocks/clocks.c
    ${PICO_SDK_PATH}/src/rp2_common/hardware_pio/pio.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_clib_interface/newlib_interface.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_platform_panic/panic.c
    ${PICO_SDK_PATH}/src/rp2_common/pico_runtime/runtime.c
    ${PICO_SDK_PATH}/lib/cyw43-driver/src/cyw43_ctrl.c
    ${PICO_SDK_PATH}/lib/cyw43-driver/src/cyw43_ll.c
    ${PICO_SDK_PATH}/lib/cyw43-driver/src/cyw43_lwip.c
    ${PICO_SDK_PATH}/lib/cyw43-driver/src/cyw43_stats.c
    ${_ria_lwip_core}
    ${PICO_SDK_PATH}/lib/lwip/src/netif/ethernet.c
    ${RP6502_ROOT}/vendor/tinyusb/src/tusb.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/tusb_fifo.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/usbh.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/hub.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/hcd_rp2350.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/rp2350_usb.c
    ${RP6502_ROOT}/vendor/fatfs/ff.c
    ${RP6502_ROOT}/src/host/pico/ria/usb/msc.c
    ${RP6502_ROOT}/src/host/pico/ria/usb/usb.c
    ${RP6502_ROOT}/src/osal/pico/fs.c
    ${RP6502_ROOT}/src/host/pico/host.c
    ${RP6502_ROOT}/src/core/api/std.c
    ${RIA_SRC}/mon/uf2_write.c
    ${RP6502_ROOT}/src/core/aud/bel.c
    ${RP6502_ROOT}/src/core/com/pick.c
    ${RIA_SRC}/sys/com.c
    ${RIA_SRC}/sys/com_telnet.c
    ${RP6502_ROOT}/src/host/pico/ria-w/net/cyw.c
    ${RP6502_ROOT}/src/host/pico/ria-w/net/net.c
    ${RP6502_ROOT}/src/host/pico/ria-w/net/telnet.c
    ${RP6502_ROOT}/src/host/pico/ria-w/net/wifi.c
)
set_source_files_properties(${RIA_RAM_SOURCES} PROPERTIES COMPILE_OPTIONS -fno-lto)
set(_ria_ram_files "*libgcc.a: *libc.a: *lib*_a-mem*.o *lib*_a-strcmp.o *lib*_a-strncmp.o *lib*_a-strncpy.o *lib*_a-strlen*.o *libm.a:")
# An object sits in a directory named as CMake names it: the source's path
# from here, with ".." turned into "__".
foreach(_src ${RIA_RAM_SOURCES})
    file(RELATIVE_PATH _dir ${CMAKE_CURRENT_SOURCE_DIR} ${_src})
    get_filename_component(_dir ${_dir} DIRECTORY)
    get_filename_component(_dir ${_dir} NAME)
    if(_dir STREQUAL "..")
        set(_dir "__")
    endif()
    get_filename_component(_name ${_src} NAME)
    string(APPEND _ria_ram_files " */${_dir}/${_name}.o")
endforeach()
set(_ria_ld ${CMAKE_CURRENT_BINARY_DIR}/${RIA_TARGET}_ld)
file(WRITE ${_ria_ld}/default_text_excludes.incl "*(EXCLUDE_FILE(${_ria_ram_files}) .text*)\n")
file(WRITE ${_ria_ld}/default_rodata_excludes.incl "*(EXCLUDE_FILE(${_ria_ram_files}) .rodata*)\n")
pico_add_linker_script_override_path(${RIA_TARGET} ${_ria_ld}
    FILES default_text_excludes.incl default_rodata_excludes.incl)

set_target_properties(${RIA_TARGET} PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION TRUE
)

# Everything that can run while the RIA rewrites its own flash must be in RAM.
# flash_check.py proves it from the ELF after every link.
add_custom_command(TARGET ${RIA_TARGET} POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${RP6502_ROOT}/src/host/pico/flash_check.py
        --elf $<TARGET_FILE:${RIA_TARGET}>
        --compile-commands ${CMAKE_BINARY_DIR}/compile_commands.json
        --objdump ${CMAKE_OBJDUMP}
        --addr2line ${CMAKE_ADDR2LINE}
        --readelf ${CMAKE_READELF}
    VERBATIM)

# Avoid 26KB of unicode and jis we don't need. Newlib's strftime calls
# sniprintf, which is wrapped to call vsnprintf so that newlib's printf
# engine is not linked beside pico_printf.
# GCC 15.2's LTO identical code folding crashes on this mix of LTO and non-LTO
# objects ("during IPA pass: icf").
target_compile_options(${RIA_TARGET} PRIVATE -fno-ipa-icf)
target_link_options(${RIA_TARGET} PRIVATE -fno-ipa-icf)

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

target_include_directories(${RIA_TARGET} PRIVATE
    ${RP6502_ROOT}/src/host/pico
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RP6502_GEN}
    ${RIA_SRC}
    ${RP6502_ROOT}/src
    ${RP6502_ROOT}/vendor
)

target_compile_definitions(${RIA_TARGET} PRIVATE
    PICO_PRINTF_SUPPORT_FLOAT=0
    PICO_PRINTF_SUPPORT_EXPONENTIAL=0
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    PICO_TIME_DEFAULT_ALARM_POOL_DISABLED=1 # sleeps spin, and there is no alarm pool interrupt
    LFS_NO_ASSERT=1
    LFS_NO_MALLOC=1
    LFS_NAME_MAX=16
    LFS_DEFINES=osal/pico/lfs_log.h
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
    ${RP6502_GEN}/unicode_oem.c
    ${RP6502_ROOT}/src/core/api/std.c
    ${RIA_SRC}/api/tim.c
    ${RIA_SRC}/aud/mix.c
    ${RP6502_ROOT}/src/core/aud/bel.c
    ${RP6502_ROOT}/src/core/aud/opl.c
    ${RP6502_ROOT}/src/core/aud/pcm.c
    ${RP6502_ROOT}/src/core/aud/psg.c
    ${RP6502_ROOT}/src/core/hid/hid.c
    ${RP6502_ROOT}/src/core/hid/keyboard.c
    ${RP6502_ROOT}/src/core/hid/layout.c
    ${RP6502_ROOT}/src/core/hid/keymap.c
    ${RP6502_GEN}/layout_db.c
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
    ${RIA_SRC}/mon/uf2_write.c
    ${RP6502_ROOT}/src/core/str/rln.c
    ${RP6502_ROOT}/src/core/str/str.c
    ${RP6502_ROOT}/src/core/sys/config.c
    ${RP6502_ROOT}/src/core/sys/random.c
    ${RP6502_ROOT}/src/core/sys/ring.c
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
    ${RP6502_ROOT}/vendor/fatfs/ff.c
    ${RP6502_ROOT}/vendor/littlefs/lfs.c
    ${RP6502_ROOT}/vendor/littlefs/lfs_util.c
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
    ${RP6502_ROOT}/vendor/tinyusb/src/class/cdc/cdc_host.c
    ${RP6502_ROOT}/vendor/tinyusb/src/host/hub.c
    ${RP6502_ROOT}/vendor/tinyusb/src/host/usbh.c
    ${RP6502_ROOT}/vendor/tinyusb/src/common/tusb_fifo.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)
target_sources(${RIA_TARGET} PRIVATE
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/hcd_rp2350.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/rp2350_usb.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/midi_host.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/cdc_host.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/hub.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/usbh.c
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/tusb_fifo.c
)
