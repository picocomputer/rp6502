# The machine, as a list of files: core's parts, plus the wiring that makes
# them one machine and the savestate engine that serializes it.
#
# Two trees build this list -- the bitstream beside it, and the verilated
# model in tests/rtl -- so the composition is written once rather than twice.
# core/rtl.cmake lists the parts and stops there, because which parts make a
# machine and in what order is a board's answer, the same way a host's
# drivers.h is in C.

include_guard(GLOBAL)

set(RP6502_POCKET_CORE ${CMAKE_CURRENT_LIST_DIR}/core)

include(${RP6502_SRC}/core/rtl.cmake)

set(RP6502_MACHINE_SOURCES
    ${RP6502_RTL_SOURCES}
    ${RP6502_POCKET_CORE}/sst_engine.sv
    ${RP6502_POCKET_CORE}/wiring.sv)
