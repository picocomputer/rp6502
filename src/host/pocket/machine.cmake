include_guard(GLOBAL)

set(RP6502_POCKET_CORE ${CMAKE_CURRENT_LIST_DIR}/core)

# gen/version.py joins core's generators before rtl.cmake makes the
# packages, so its header and its package are made with theirs.
include(${RP6502_SRC}/core/gen.cmake)
list(APPEND RP6502_GENERATORS ${CMAKE_CURRENT_LIST_DIR}/gen/version.py)
include(${RP6502_SRC}/core/rtl.cmake)

set(RP6502_MACHINE_SOURCES
    ${RP6502_RTL_SOURCES}
    ${RP6502_ASSETS}/pocket_version_pkg.sv
    ${RP6502_POCKET_CORE}/wiring.sv)
