# The 65C02 and its VIA.
#
# The emulator's CPU and the FPGA core's answer the same dut.h, so one set of
# suites holds both: vec.c and klaus.c are the evidence, chips_dut.c and
# w65c02_dut.cpp are the two implementations under it.

set(CPU_DUT ${CMAKE_CURRENT_LIST_DIR}/chips_dut.c)
set(CPU_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}
    ${RP6502_VENDOR})

# --- SingleStepTests: per-cycle bus traces, one instruction per case ---
# Upstream carries five CPU families, so SPARSE takes the one we run.
rp6502_submodule(vendor/65x02 SENTINEL wdc65c02/v1/00.json
    SPARSE wdc65c02/v1 TARGET vectors-download OPTIONAL
    WANTS "per-cycle conformance" RESULT RP6502_HAVE_SSTESTS)
set(SSTESTS ${RP6502_ROOT}/vendor/65x02/wdc65c02/v1)
set(SSVEC_COUNT 100 CACHE STRING "SingleStepTests cases per opcode")

if(RP6502_HAVE_SSTESTS)
    set(VECTORS ${CMAKE_CURRENT_BINARY_DIR}/wdc65c02.vec)
    add_custom_command(OUTPUT ${VECTORS}
        COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_LIST_DIR}/ssvec.py
            --src ${SSTESTS} --out ${VECTORS} --count ${SSVEC_COUNT}
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/ssvec.py
        COMMENT "Encoding SingleStepTests vectors (${SSVEC_COUNT}/opcode)"
        VERBATIM)
    add_custom_target(cpu_vectors DEPENDS ${VECTORS})

    rp6502_add_test(vectors
        SOURCES test_vectors.c vec.c ${CPU_DUT}
        INCLUDES ${CPU_INCLUDES}
        DEFS VECTORS="${VECTORS}"
        TIMEOUT 300)
    add_dependencies(test_vectors cpu_vectors)
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

if(RP6502_HAVE_KLAUS)
    rp6502_add_test(klaus
        SOURCES test_klaus.c klaus.c ${CPU_DUT}
        INCLUDES ${CPU_INCLUDES}
        DEFS
            KLAUS_6502="${KLAUS}/6502_functional_test.bin"
            KLAUS_65C02="${KLAUS}/65C02_extended_opcodes_test.bin"
        TIMEOUT 300)
else()
    message(WARNING "Klaus Dormann images absent — functional soak skipped.")
endif()

# --- The vendored 65C02 on its own ---
# The corpora above say the CPU is right; this says it has not changed, which
# is a weaker claim that survives a checkout without them. It also covers the
# pins, which neither corpus exercises.
rp6502_add_test(w65c02_chips
    SOURCES test_w65c02_chips.c lockstep_scen.c ${CPU_DUT}
    INCLUDES ${CPU_INCLUDES}
    DEFS W65C02_GOLDEN="${CMAKE_CURRENT_LIST_DIR}/w65c02_golden.txt")

# --- The vendored VIA on its own ---
# chips/chips/m6522.h is someone else's code the emulator depends on, and
# until this existed its only test needed Verilator — so an emulator build
# ran nothing at all against it. Same scenarios as the RTL comparison, held
# to a recorded trace instead of to w65c22.sv.
rp6502_add_test(w65c22_chips
    SOURCES test_w65c22_chips.c w65c22_scen.c
    INCLUDES ${CPU_INCLUDES}
    DEFS W65C22_GOLDEN="${CMAKE_CURRENT_LIST_DIR}/w65c22_golden.txt")

if(RP6502_VERILATE)
    # --- The same two suites again, against the verilated w65c02 ---
    if(VECTORS AND RP6502_HAVE_KLAUS)
        rp6502_add_module_test(w65c02
            TOP w65c02
            RTL ${W65C02_ROM} ${RP6502_SRC}/rtl/wdc/w65c02.sv
            SOURCES test_w65c02.cpp w65c02_dut.cpp vec.c klaus.c
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
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
    # Its scenarios are written here, so it wants no corpus and is not gated
    # on one.
    rp6502_add_module_test(lockstep
        TOP w65c02
        RTL ${W65C02_ROM} ${RP6502_SRC}/rtl/wdc/w65c02.sv
        SOURCES test_lockstep.cpp w65c02_dut.cpp chips_dut.c lockstep.c
            lockstep_scen.c
        INCLUDES ${CPU_INCLUDES}
        DEPENDS w65c02_rom
        TIMEOUT 300)

    # --- Stopped here, started again there ---
    # A savestate's whole claim about the 6502: the core that comes back
    # continues rather than restarts, cycle for cycle.
    rp6502_add_module_test(freeze
        TOP w65c02
        RTL ${W65C02_ROM} ${RP6502_SRC}/rtl/wdc/w65c02.sv
        DEPENDS w65c02_rom
        TIMEOUT 300)

    # --- The VIA in RTL, against chips/m6522.h in lockstep ---
    # T1/T2 timers and their interrupts are live machine features; see
    # emu/emu/via.c for the wiring this reproduces.
    rp6502_add_module_test(w65c22
        TOP w65c22
        RTL ${RP6502_SRC}/rtl/wdc/w65c22.sv
        SOURCES test_w65c22.cpp w65c22_scen.c
        INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${RP6502_VENDOR}
        TIMEOUT 300)
endif()
