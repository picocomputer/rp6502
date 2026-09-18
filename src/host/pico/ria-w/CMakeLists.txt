set(RIA_TARGET rp6502-ria-w)
include(${RP6502_ROOT}/src/host/pico/ria.cmake)

pico_set_program_name(${RIA_TARGET} "RP6502-RIA-W")

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

target_compile_options(${RIA_TARGET} PRIVATE $<$<CONFIG:Debug>:-Os>)

set_source_files_properties(
    ${PICO_SDK_PATH}/src/rp2_common/pico_btstack/btstack_flash_bank.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)
