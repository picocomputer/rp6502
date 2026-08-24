# XRAM and the RIA's data ports into it.
#
# Wholly a machine claim: the emulator's memory is an array, and what is worth
# asking is whether the fabric's answers it. So there is no tests/emu/mem.

if(RP6502_SOFT_CPU)
    # XRAM records through the staged loader, judged by the oracle.
    rp6502_add_machine_test(xram ORACLE FIRMWARE TIMEOUT 900)

    # RW0/RW1 steps, wraps and defaults against the oracle.
    rp6502_add_machine_test(rw ORACLE FIRMWARE TIMEOUT 900)
endif()
