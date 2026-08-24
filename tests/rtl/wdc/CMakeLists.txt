# The 65C02 and its VIA in the fabric, against the emulator's.
#
# Nothing here is a second copy of the evidence. dut.h is the interface both
# implementations answer, vec.c and klaus.c are the corpora, chips_dut.c is the
# emulator's CPU under them — all of it in tests/emu/wdc, compiled from here
# against w65c02_dut.cpp, which is this one's. Holding the two to one standard
# is the whole point; holding them to two copies of one would not be.

set(EMU_WDC ${RP6502_TESTS_DIR}/emu/wdc)
set(CPU_INCLUDES ${EMU_WDC} ${CMAKE_CURRENT_LIST_DIR} ${RP6502_VENDOR})

# --- SingleStepTests: per-cycle bus traces, one instruction per case ---
# Upstream carries five CPU families, so SPARSE takes the one we run. Asked
# for again rather than inherited: this is a tree of its own, and a corpus
# absent here is a suite skipped here.
rp6502_submodule(vendor/65x02 SENTINEL wdc65c02/v1/00.json
    SPARSE wdc65c02/v1 TARGET vectors-download OPTIONAL
    WANTS "per-cycle conformance" RESULT RP6502_HAVE_SSTESTS)
set(SSTESTS ${RP6502_ROOT}/vendor/65x02/wdc65c02/v1)
set(SSVEC_COUNT 100 CACHE STRING "SingleStepTests cases per opcode")

if(RP6502_HAVE_SSTESTS)
    set(VECTORS ${CMAKE_CURRENT_BINARY_DIR}/wdc65c02.vec)
    add_custom_command(OUTPUT ${VECTORS}
        COMMAND ${CMAKE_COMMAND} -E env python3
            ${EMU_WDC}/ssvec.py
            --src ${SSTESTS} --out ${VECTORS} --count ${SSVEC_COUNT}
        DEPENDS ${EMU_WDC}/ssvec.py
        COMMENT "Encoding SingleStepTests vectors (${SSVEC_COUNT}/opcode)"
        VERBATIM)
    add_custom_target(cpu_vectors DEPENDS ${VECTORS})
else()
    message(WARNING "SingleStepTests corpus absent — per-cycle conformance"
        " skipped.")
endif()

# --- Klaus Dormann: long self-checking sequences ---
# GPL-3.0, so it stays a submodule reference rather than bytes in this BSD-3
# tree. The images are data the tests read; nothing is linked or shipped.
rp6502_submodule(vendor/6502_65C02_functional_tests
    SENTINEL bin_files/6502_functional_test.bin
    WANTS "the functional soak" OPTIONAL RESULT RP6502_HAVE_KLAUS)
set(KLAUS ${RP6502_ROOT}/vendor/6502_65C02_functional_tests/bin_files)

# --- The same two corpora, against the verilated w65c02 ---
if(VECTORS AND RP6502_HAVE_KLAUS)
    rp6502_add_module_test(w65c02
        TOP w65c02
        RTL ${W65C02_ROM} ${RP6502_SRC}/core/wdc/w65c02.sv
        SOURCES test_w65c02.cpp w65c02_dut.cpp ${EMU_WDC}/vec.c ${EMU_WDC}/klaus.c
        INCLUDES ${CPU_INCLUDES}
        DEFS
            VECTORS="${VECTORS}"
            KLAUS_6502="${KLAUS}/6502_functional_test.bin"
            KLAUS_65C02="${KLAUS}/65C02_extended_opcodes_test.bin"
        DEPENDS cpu_vectors w65c02_rom
        TIMEOUT 900)
else()
    message(WARNING "SingleStepTests or Klaus absent — w65c02 RTL"
        " conformance skipped.")
endif()

# Interrupts, reset, RDY, WAI and STP: the C emulation and the RTL in
# lockstep under scripted pins — coverage the vectors cannot express.
# Its scenarios are written beside the emulator's CPU, so it wants no corpus
# and is not gated on one.
rp6502_add_module_test(lockstep
    TOP w65c02
    RTL ${W65C02_ROM} ${RP6502_SRC}/core/wdc/w65c02.sv
    SOURCES test_lockstep.cpp w65c02_dut.cpp
        ${EMU_WDC}/chips_dut.c ${EMU_WDC}/lockstep.c ${EMU_WDC}/lockstep_scen.c
    INCLUDES ${CPU_INCLUDES}
    DEPENDS w65c02_rom
    TIMEOUT 300)

# --- Stopped here, started again there ---
# A savestate's whole claim about the 6502: the core that comes back
# continues rather than restarts, cycle for cycle.
rp6502_add_module_test(freeze
    TOP w65c02
    RTL ${W65C02_ROM} ${RP6502_SRC}/core/wdc/w65c02.sv
    DEPENDS w65c02_rom
    TIMEOUT 300)

# --- The VIA in RTL, against chips/m6522.h in lockstep ---
# T1/T2 timers and their interrupts are live machine features; see
# emu/emu/via.c for the wiring this reproduces.
rp6502_add_module_test(w65c22
    TOP w65c22
    RTL ${RP6502_SRC}/core/wdc/w65c22.sv
    SOURCES test_w65c22.cpp ${EMU_WDC}/w65c22_scen.c
    INCLUDES ${CPU_INCLUDES}
    TIMEOUT 300)
