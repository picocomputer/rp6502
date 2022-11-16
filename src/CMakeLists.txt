add_executable(rp6502_ria)
pico_add_extra_outputs(rp6502_ria)
pico_set_program_name(rp6502_ria "RP6502 RIA")
pico_set_binary_type(rp6502_ria copy_to_ram)

add_subdirectory(_tinyusb)
target_include_directories(rp6502_ria PRIVATE _tinyusb)

target_link_libraries(rp6502_ria PRIVATE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    _tinyusb
)

pico_generate_pio_header(rp6502_ria
    ${CMAKE_CURRENT_LIST_DIR}/ria.pio
)

target_sources(rp6502_ria PRIVATE
    hid_ps4.c
    hid.c
    main.c
    mon.c
    msc.c
    ria.c
)


add_executable(rp6502_vga)
pico_add_extra_outputs(rp6502_vga)
pico_set_program_name(rp6502_vga "RP6502 VGA")
pico_set_binary_type(rp6502_vga copy_to_ram)

target_include_directories(rp6502_vga PRIVATE probe)

target_sources(rp6502_vga PRIVATE
    term.c
    vga.c
    ria.c
    probe/main.c
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
    tinyusb_device
)

pico_generate_pio_header(rp6502_vga
    ${CMAKE_CURRENT_LIST_DIR}/probe/probe.pio
)

target_compile_definitions(rp6502_vga PRIVATE
    PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=323
    PICO_SCANVIDEO_COLOR_PIN_BASE=6
    PICO_SCANVIDEO_SYNC_PIN_BASE=26
)
