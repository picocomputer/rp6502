# The soft CPU: the Hazard3 SoC the firmware runs on.

if(RP6502_VERILATE)
    # Its microsecond clock against clk_sys wall time, which has now been
    # wrong twice.
    rp6502_add_machine_test(mtime TIMEOUT 120)

    # A load of the word a store just wrote, which the AHB overlaps onto
    # one edge and the block RAM answers stale.
    rp6502_add_machine_test(tcm TIMEOUT 120)
endif()
