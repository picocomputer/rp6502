include_guard(GLOBAL)

set(RP6502_POCKET_CORE ${CMAKE_CURRENT_LIST_DIR}/core)

include(${RP6502_SRC}/core/rtl.cmake)

set(RP6502_MACHINE_SOURCES
    ${RP6502_RTL_SOURCES}
    ${RP6502_POCKET_CORE}/sst_engine.sv
    ${RP6502_POCKET_CORE}/wiring.sv)
