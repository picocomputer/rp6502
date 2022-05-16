add_executable(rp6502)
pico_add_extra_outputs(rp6502)
pico_set_program_name(rp6502 "RP6502")

include_directories( _tinyusb )
add_subdirectory(_tinyusb)

target_link_libraries(rp6502 PRIVATE
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_pio
    pico_scanvideo_dpi
    _tinyusb
)

target_compile_definitions(rp6502 PRIVATE
    PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=323
    PICO_SCANVIDEO_COLOR_PIN_BASE=2
    PICO_SCANVIDEO_COLOR_PIN_COUNT=12
    PICO_SCANVIDEO_PIXEL_RCOUNT=4
    PICO_SCANVIDEO_PIXEL_GCOUNT=4
    PICO_SCANVIDEO_PIXEL_BCOUNT=4
    PICO_SCANVIDEO_PIXEL_RSHIFT=0u
    PICO_SCANVIDEO_PIXEL_GSHIFT=4u
    PICO_SCANVIDEO_PIXEL_BSHIFT=8u
)

pico_generate_pio_header(rp6502
    ${CMAKE_CURRENT_LIST_DIR}/dm65.pio
)

target_sources(rp6502 PRIVATE
    dm65.c
    hid_ps4.c
    hid.c
    main.c
    mon.c
    msc.c
    term.c
    vga.c
)
