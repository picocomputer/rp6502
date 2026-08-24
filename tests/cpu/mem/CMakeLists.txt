# XRAM and the RIA's data ports into it.

# RW0/RW1 steps, wraps and defaults, on whichever machine this tree built.
# One source, one expectation; the fabric needs its firmware to boot an image,
# so only that tree registers it there.
if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(rw FIRMWARE
            SOURCES test_rw.cpp ${RP6502_BENCH}/mut_rtl.cpp
            TIMEOUT 900)
    endif()
else()
    rp6502_add_test(rw LIBS emu_core
        SOURCES test_rw.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        TIMEOUT 60)
endif()

# XRAM records through the staged loader, against the image written out in
# the test rather than against another machine.
if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(xram FIRMWARE
            SOURCES test_xram.cpp ${RP6502_BENCH}/mut_rtl.cpp
            TIMEOUT 900)
    endif()
else()
    rp6502_add_test(xram LIBS emu_core
        SOURCES test_xram.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        TIMEOUT 60)
endif()
