# The RP6502-VGA

# The firmware's own sources; this file is the parts list, they are the parts.
set(VGA_SRC ${RP6502_ROOT}/src/host/pico/vga)

add_executable(rp6502-vga)
target_compile_definitions(rp6502-vga PRIVATE ${RP6502_PROJECT_DEFINITIONS}
    # This image formats no float at all; the pico printf carries
    # _ftoa/_etoa unconditionally otherwise.
    PICO_PRINTF_SUPPORT_FLOAT=0
    PICO_PRINTF_SUPPORT_EXPONENTIAL=0
)
pico_add_extra_outputs(rp6502-vga)
pico_set_program_name(rp6502-vga "RP6502-VGA")
pico_set_binary_type(rp6502-vga copy_to_ram)

set_target_properties(rp6502-vga PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION TRUE
)

target_link_options(rp6502-vga PRIVATE ${IPO_PRINTF_LINK_OPTIONS})

target_compile_options(rp6502-vga PRIVATE
    -Wall -Wextra -Wsign-compare
)

target_include_directories(rp6502-vga PRIVATE
    ${RP6502_ROOT}/src/host/pico  # the pico's own headers
    ${VGA_SRC}                    # this machine: drivers.h and its sources
    ${RP6502_ROOT}/src
    ${RP6502_ROOT}/vendor         # host.c names littlefs's CRC; unlinked here
)

# Core 1 renders continuously and never reads HOST_IN_FLASH data -- the glyph
# tables it walks are the RAM copies font_init fills -- so XIP being down for
# a sector write does not reach it. (PICO_FLASH_ASSUME_CORE1_SAFE would say
# that to pico_flash; this target links hardware_flash and calls it directly,
# so the invariant is stated here instead of asserted there.)

target_sources(rp6502-vga PRIVATE
    ${VGA_SRC}/main.c
    ${RP6502_ROOT}/src/host/pico/host.c
    ${RP6502_ROOT}/src/core/vga/prog.c
    ${RP6502_ROOT}/src/core/vga/mode/mode.c
    ${RP6502_ROOT}/src/core/vga/mode/mode0.c
    ${RP6502_ROOT}/src/core/vga/mode/mode1.c
    ${RP6502_ROOT}/src/core/vga/mode/mode2.c
    ${RP6502_ROOT}/src/core/vga/mode/mode3.c
    ${RP6502_ROOT}/src/core/vga/mode/mode4.c
    ${RP6502_ROOT}/src/core/vga/mode/mode5.c
    ${VGA_SRC}/scanvideo/scanvideo.c
    ${RP6502_ROOT}/src/core/sys/version.c
    ${VGA_SRC}/sys/com.c
    ${VGA_SRC}/sys/flash.c
    ${VGA_SRC}/sys/led.c
    ${RP6502_ROOT}/src/core/sys/xram.c
    ${VGA_SRC}/sys/pix.c
    ${VGA_SRC}/sys/ria.c
    ${VGA_SRC}/sys/vga.c
    ${RP6502_ROOT}/src/core/term/color.c
    ${RP6502_ROOT}/src/core/term/font.c
    ${RP6502_ROOT}/src/core/term/term.c
    ${VGA_SRC}/usb/cdc.c
    ${VGA_SRC}/usb/descriptors.c
    ${VGA_SRC}/usb/usb.c
)

rp6502_use_version_header(rp6502-vga
    ${RP6502_ROOT}/src/core/sys/version.c
)

target_link_libraries(rp6502-vga PRIVATE
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_pio
    hardware_dma
    hardware_flash
    hardware_interp
    tinyusb_device
)

pico_generate_pio_header(rp6502-vga
    ${VGA_SRC}/vga.pio
)

pico_generate_pio_header(rp6502-vga
    ${VGA_SRC}/scanvideo/scanvideo.pio
)

# Override various lib files with custom code.
# The magic is that header-only files don't get compiled.
set_source_files_properties(
    ${RP6502_ROOT}/vendor/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)
target_sources(rp6502-vga PRIVATE
    ${RP6502_ROOT}/vendor/tinyusb_rp6502/rp2040_usb.c
)
