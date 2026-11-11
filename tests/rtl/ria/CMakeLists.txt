rp6502_add_machine_test(xregs FIRMWARE
    SOURCES test_xregs.cpp
    INCLUDES ${RP6502_TESTS_DIR}/cpu/ria
    TIMEOUT 900)

rp6502_add_module_test(rxregs
    TOP regs
    RTL ${RP6502_SRC}/core/ria/regs.sv
    ARGS -Wno-VARHIDDEN
    TIMEOUT 120)

rp6502_add_module_test(irqregs
    TOP regs
    RTL ${RP6502_SRC}/core/ria/regs.sv
    ARGS -Wno-VARHIDDEN
    TIMEOUT 120)
