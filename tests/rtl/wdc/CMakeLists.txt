set(CPU_WDC ${RP6502_TESTS_DIR}/cpu/wdc)

rp6502_add_module_test(phi2
    TOP phi2
    RTL ${RP6502_SRC}/core/wdc/phi2.sv
    TIMEOUT 120)

rp6502_add_module_test(lockstep
    TOP cpu
    RTL ${W65C02_ROM} ${RP6502_SRC}/core/wdc/cpu.sv
    SOURCES test_lockstep.cpp ${CPU_WDC}/rtl_dut.cpp
        ${CPU_WDC}/chips_dut.c lockstep.c ${CPU_WDC}/lockstep_scen.c
    INCLUDES ${CPU_WDC} ${CMAKE_CURRENT_LIST_DIR} ${RP6502_VENDOR}
    DEPENDS w65c02_rom
    TIMEOUT 300)

rp6502_add_module_test(freeze
    TOP cpu
    RTL ${W65C02_ROM} ${RP6502_SRC}/core/wdc/cpu.sv
    DEPENDS w65c02_rom
    TIMEOUT 300)
