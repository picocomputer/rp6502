# The RP6502-VGA

# The firmware's own sources; this file is the parts list, they are the parts.
set(VGA_SRC ${RP6502_ROOT}/src/host/pico/vga)

add_executable(rp6502-vga)
target_compile_definitions(rp6502-vga PRIVATE ${RP6502_PROJECT_DEFINITIONS})
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
    ${RP6502_ROOT}/src/host/pico  # host.h, and this host's own
    ${VGA_SRC}
    ${RP6502_ROOT}/src
)

target_compile_definitions(rp6502-vga PRIVATE 
    PICO_FLASH_ASSUME_CORE1_SAFE=1
)

target_sources(rp6502-vga PRIVATE
    ${VGA_SRC}/main.c
    ${RP6502_ROOT}/src/host/pico/host.c
    ${RP6502_ROOT}/src/core/vga/prog.c
    ${RP6502_ROOT}/src/core/vga/mode.c
    ${RP6502_ROOT}/src/core/vga/mode0.c
    ${RP6502_ROOT}/src/core/vga/mode1.c
    ${RP6502_ROOT}/src/core/vga/mode2.c
    ${RP6502_ROOT}/src/core/vga/mode3.c
    ${RP6502_ROOT}/src/core/vga/mode4.c
    ${RP6502_ROOT}/src/core/vga/mode5.c
    ${VGA_SRC}/scanvideo/scanvideo.c
    ${VGA_SRC}/sys/com.c
    ${VGA_SRC}/sys/led.c
    ${VGA_SRC}/sys/mem.c
    ${VGA_SRC}/sys/pix.c
    ${VGA_SRC}/sys/ria.c
    ${VGA_SRC}/sys/sys.c
    ${VGA_SRC}/sys/vga.c
    ${RP6502_ROOT}/src/core/term/color.c
    ${RP6502_ROOT}/src/core/term/font.c
    ${RP6502_ROOT}/src/core/term/term.c
    ${VGA_SRC}/usb/cdc.c
    ${VGA_SRC}/usb/descriptors.c
    ${VGA_SRC}/usb/usb.c
)

rp6502_use_version_header(rp6502-vga
    ${VGA_SRC}/sys/sys.c
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
