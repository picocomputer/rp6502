# The RP2350 firmware: the machines that are chips on the Picocomputer's board.
#
# The repo root above is the Pico VS Code extension's file and the SDK's entry
# point; what the firmware itself is made of is here. Two machines are built:
# the VGA always, and whichever RIA the board has.

# Both images say the same things about themselves before anything can take a
# default. Change the name for a hardware fork; the creator flag hides MAC and
# SSID; RP6502_LOCALE is the build default locale.
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

include(${RP6502_ROOT}/submodules.cmake)
rp6502_submodule(vendor/littlefs SENTINEL lfs.c
    WANTS "the firmware's flash filesystem")

include(${RP6502_ROOT}/version.cmake)

# The radio is a board, not a build flag: each RIA is its own machine with its
# own driver roster and its own parts, and the board says which one this is.
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

# Each machine builds where every other machine on this tree does, at
# build/<machine>, rather than under a directory named for the family.
add_subdirectory(vga ${CMAKE_BINARY_DIR}/vga)
add_subdirectory(${_ria} ${CMAKE_BINARY_DIR}/${_ria})
