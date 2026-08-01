# The soft CPU: the Hazard3 SoC the firmware runs on.

if(RP6502_VERILATE)
    # Its microsecond clock against clk_sys wall time, which has now been
    # wrong twice.
    rp6502_add_machine_test(mtime TIMEOUT 120)
endif()
