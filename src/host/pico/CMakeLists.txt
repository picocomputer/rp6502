set(RP6502_PROJECT_DEFINITIONS
    RP6502_NAME="Picocomputer 6502"
    RP6502_LOCALE=EN
    RP6502_EXFAT=0
    RP6502_CREATOR=0
)

# Force wrappers defined by pico_printf and pico_stdlib to be
# preserved during link-time interprocedural optimizations.
set(IPO_PRINTF_LINK_OPTIONS
    -Wl,-u,__wrap_printf
    -Wl,-u,__wrap_vprintf
    -Wl,-u,__wrap_puts
    -Wl,-u,__wrap_putchar
    -Wl,-u,__wrap_getchar
    -Wl,-u,__wrap_sprintf
    -Wl,-u,__wrap_snprintf
    -Wl,-u,__wrap_vsnprintf
)

# Every Pico board keeps XRAM in the last 64 KB of main RAM, and the RIA keeps
# its registers through a reboot; see ld/, which places xram by name.
function(rp6502_pico_memory TARGET)
    pico_add_linker_script_override_path(${TARGET} ${RP6502_ROOT}/src/host/pico/ld GLOB_FILES)
    rp6502_linker_named(${TARGET} ${RP6502_ROOT}/src/core/sys/xram.c)
endfunction()

# The console on a debug probe, for the bench; see rtt.h. A release has none.
option(RP6502_RTT_DEBUG "Put the console on a debug probe, as SEGGER RTT" OFF)
function(rp6502_pico_rtt TARGET)
    if(RP6502_RTT_DEBUG)
        target_sources(${TARGET} PRIVATE ${RP6502_ROOT}/src/host/pico/rtt.c)
        target_compile_definitions(${TARGET} PRIVATE RP6502_RTT_DEBUG=1)
    endif()
endfunction()

include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/littlefs SENTINEL lfs.c
    WANTS "the firmware's flash filesystem")

include(${RP6502_ROOT}/version.cmake)

# If you have thoroughly tested a third-party RP2350 board in the reference
# design then please submit a pull request.
if(PICO_BOARD STREQUAL "pico2_w" OR
   PICO_BOARD STREQUAL "pimoroni_pico_plus2_w_rp2350")
    set(_ria ria-w)
elseif(PICO_BOARD STREQUAL "pico2")
    set(_ria ria)
else()
    message(FATAL_ERROR "Unknown board: ${PICO_BOARD}")
endif()

add_subdirectory(vga ${CMAKE_BINARY_DIR}/vga)
add_subdirectory(${_ria} ${CMAKE_BINARY_DIR}/${_ria})
