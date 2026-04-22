# The RP6502-VGA

add_executable(rp6502_vga)
target_compile_definitions(rp6502_vga PRIVATE ${RP6502_PROJECT_DEFINITIONS})
pico_add_extra_outputs(rp6502_vga)
pico_set_program_name(rp6502_vga "RP6502-VGA")
pico_set_binary_type(rp6502_vga copy_to_ram)

set_target_properties(rp6502_vga PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION TRUE
)

target_link_options(rp6502_vga PRIVATE ${IPO_PRINTF_LINK_OPTIONS})

target_compile_options(rp6502_vga PRIVATE
    -Wall -Wextra -Wsign-compare
)

target_include_directories(rp6502_vga PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_sources(rp6502_vga PRIVATE
    main.c
    modes/mode1.c
    modes/mode2.c
    modes/mode3.c
    modes/mode4.c
    modes/mode5.c
    scanvideo/scanvideo.c
    sys/com.c
    sys/led.c
    sys/mem.c
    sys/pix.c
    sys/ria.c
    sys/sys.c
    sys/vga.c
    term/color.c
    term/font.c
    term/term.c
    usb/cdc.c
    usb/descriptors.c
    usb/usb.c
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
    ${CMAKE_CURRENT_LIST_DIR}/vga.pio
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/scanvideo/scanvideo.pio
)

# Override various lib files with custom code.
# The magic is that header-only files don't get compiled.
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/src/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)
target_sources(rp6502_vga PRIVATE
    ${PROJECT_SOURCE_DIR}/src/tinyusb_rp6502/rp2040_usb.c
)

# If no version set, set a define for C preprocessor and touch
# the file that provides a dev timestamp for the version.
get_target_property(RP6502_VGA_COMPILE_DEFINITIONS rp6502_vga COMPILE_DEFINITIONS)
if(RP6502_VGA_COMPILE_DEFINITIONS MATCHES "RP6502_VERSION=\"\"")
    set_property(TARGET rp6502_vga
        APPEND PROPERTY COMPILE_DEFINITIONS
        RP6502_VERSION_EMPTY=1
    )
    add_custom_command(TARGET rp6502_vga PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_LIST_DIR}/sys/sys.c
    )
endif()
