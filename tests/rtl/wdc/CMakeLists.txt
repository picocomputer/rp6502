# The 65C02 in the fabric, by methods only a simulator has.
#
# The corpora, the recorded traces and the scenarios are in tests/cpu/wdc,
# where both implementations answer them — lockstep.h among them, because the
# scenarios are written against it. What is here needs the model itself: pins
# driven cycle by cycle against the C emulation running beside it, and a core
# stopped and restarted mid-instruction.

set(CPU_WDC ${RP6502_TESTS_DIR}/cpu/wdc)

# --- Every whole kilohertz, exactly ---
# The clock the rest of this directory runs on. tests/cpu/ria/test_clock.c
# makes the same claim of the C accumulator.
rp6502_add_module_test(phi2
    TOP phi2
    RTL ${RP6502_SRC}/core/wdc/phi2.sv
    TIMEOUT 120)

# Interrupts, reset, RDY, WAI and STP: the C emulation and the RTL in
# lockstep under scripted pins — coverage the vectors cannot express.
# Its scenarios are the ones cpu/wdc holds, so it wants no corpus and is
# not gated on one.
rp6502_add_module_test(lockstep
    TOP cpu
    RTL ${W65C02_ROM} ${RP6502_SRC}/core/wdc/cpu.sv
    SOURCES test_lockstep.cpp ${CPU_WDC}/rtl_dut.cpp
        ${CPU_WDC}/chips_dut.c lockstep.c ${CPU_WDC}/lockstep_scen.c
    INCLUDES ${CPU_WDC} ${CMAKE_CURRENT_LIST_DIR} ${RP6502_VENDOR}
    DEPENDS w65c02_rom
    TIMEOUT 300)

# --- Stopped here, started again there ---
# A savestate's whole claim about the 6502: the core that comes back
# continues rather than restarts, cycle for cycle.
rp6502_add_module_test(freeze
    TOP cpu
    RTL ${W65C02_ROM} ${RP6502_SRC}/core/wdc/cpu.sv
    DEPENDS w65c02_rom
    TIMEOUT 300)
