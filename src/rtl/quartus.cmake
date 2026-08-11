find_program(QUARTUS_MAP quartus_map HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_FIT quartus_fit HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_STA quartus_sta HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_ASM quartus_asm HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_DRC quartus_drc HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_CDB quartus_cdb HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)

set(RP6502_SDC ${RP6502_SRC}/host/pocket/quartus/rp6502.sdc)
