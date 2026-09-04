# The RP6502-RIA-W: the same board with the radio populated.
#
# Everything the two boards share is the parts list one directory up. What is
# here is what having a radio costs: nine more drivers -- their sources are in
# this directory, because they are this board's -- and the two stacks that
# drive them.

set(RIA_TARGET rp6502-ria-w)
include(${RP6502_ROOT}/src/host/pico/ria.cmake)

pico_set_program_name(${RIA_TARGET} "RP6502-RIA-W")

# A few nexus points still ask -- the console's telnet fan-out, the status
# display, the LED that is wired to the radio chip rather than a pin.
target_compile_definitions(${RIA_TARGET} PRIVATE
    RP6502_RIA_W=1
    CYW43_CONFIG_FILE="${CMAKE_CURRENT_SOURCE_DIR}/cyw43_config.h"
)

target_sources(${RIA_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ble/ble.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ble/tlv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/cmd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/cyw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/modem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/net.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/ntp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/telnet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/net/wifi.c
)

target_link_libraries(${RIA_TARGET} PRIVATE
    pico_cyw43_arch_lwip_poll
    pico_btstack_cyw43
    pico_btstack_ble
)

# This one does not fit in the flash it is given at -O2.
target_compile_options(${RIA_TARGET} PRIVATE $<$<CONFIG:Debug>:-Os>)

# The RIA owns its flash; btstack's bank would write to it behind us.
# Header-only is how a vendored file is dropped without patching it.
set_source_files_properties(
    ${PICO_SDK_PATH}/src/rp2_common/pico_btstack/btstack_flash_bank.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)
