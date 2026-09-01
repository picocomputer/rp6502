# The RP6502-RIA: the reference board, no radio populated.

set(RIA_TARGET rp6502-ria)
include(${RP6502_ROOT}/src/host/pico/ria.cmake)

pico_set_program_name(${RIA_TARGET} "RP6502-RIA")
