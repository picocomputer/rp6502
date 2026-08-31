# The Quartus command line, found once. Every project this tree builds runs the
# same five executables, and rp6502.sdc is the machine's own constraints, which
# is why it sits with the machine. What a board adds to them is that board's --
# see host/pocket/quartus for the Pocket's.
#
# A tree with no Quartus still configures; it just registers no synthesis.

find_program(QUARTUS_MAP quartus_map HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_FIT quartus_fit HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_STA quartus_sta HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_ASM quartus_asm HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_DRC quartus_drc HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_CDB quartus_cdb HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)

set(RP6502_SDC ${RP6502_SRC}/core/machine/rp6502.sdc)
