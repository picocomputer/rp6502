# The 65C02, its VIA, and the clock that runs them.
#
# The emulator's CPU and the FPGA core's answer the same dut.h, so one set of
# suites holds both: vec.c and klaus.c are the evidence, chips_dut.c and
# cpu65_dut.cpp are the two implementations under it. Each suite is guarded on
# its own fixtures — the Klaus images arrive with the rest, the
# SingleStepTests corpus is asked for by a target because of its size.

set(CPU_DUT ${CMAKE_CURRENT_LIST_DIR}/chips_dut.c)
set(CPU_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}
    ${RP6502_VENDOR}/chips_rp6502 # shadows vendor/chips; must precede it
    ${RP6502_VENDOR})

# --- SingleStepTests: per-cycle bus traces, one instruction per case ---
# Upstream is five CPU families and 4.8 GB where we run one of them, so this
# one is asked for rather than fetched: a gigabyte is not something to spend
# during a configure, and a fixture that arrives at build time cannot be
# registered as a test by the configure that has already finished. So the
# corpus is offered by a target, and the configure after it sees the suite.
rp6502_submodule(vendor/65x02 SENTINEL wdc65c02/v1/00.json
    SPARSE wdc65c02/v1 LAZY vectors-download
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
        " skipped. The line above says how to fetch it.")
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

# --- Master clock + PHI2 in the emulator (reproducible run timing) ---
rp6502_add_test(clock LIBS emu_core FIXTURE adventure.rp6502)

if(RP6502_VERILATE)
    # --- The same two suites again, against the verilated cpu65 ---
    if(VECTORS AND RP6502_HAVE_KLAUS)
        rp6502_add_module_test(cpu65
            TOP cpu65
            RTL ${CPU65_ROM} ${RP6502_SRC}/rtl/cpu65/cpu65.sv
            SOURCES test_cpu65.cpp cpu65_dut.cpp vec.c klaus.c
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            DEFS
                VECTORS="${VECTORS}"
                KLAUS_6502="${KLAUS}/6502_functional_test.bin"
                KLAUS_65C02="${KLAUS}/65C02_extended_opcodes_test.bin"
            DEPENDS cpu_vectors cpu65_rom
            TIMEOUT 900)

        # Interrupts, reset, RDY, WAI and STP: the C emulation and the RTL in
        # lockstep under scripted pins — coverage the vectors cannot express.
        rp6502_add_module_test(lockstep
            TOP cpu65
            RTL ${CPU65_ROM} ${RP6502_SRC}/rtl/cpu65/cpu65.sv
            SOURCES test_lockstep.cpp cpu65_dut.cpp chips_dut.c lockstep.c
            INCLUDES ${CPU_INCLUDES}
            DEPENDS cpu65_rom
            TIMEOUT 300)
    else()
        message(WARNING "CPU fixtures absent — cpu65 RTL conformance skipped.")
    endif()

    # --- The VIA in RTL, against chips/m6522.h in lockstep ---
    # T1/T2 timers and their interrupts are live machine features; see
    # emu/emu/via.c for the wiring this reproduces.
    rp6502_add_module_test(via
        TOP via
        RTL ${RP6502_SRC}/rtl/cpu65/via.sv
        INCLUDES ${RP6502_VENDOR}/chips_rp6502 ${RP6502_VENDOR}
        TIMEOUT 300)

    # --- Every whole kilohertz, exactly ---
    rp6502_add_module_test(phi2
        TOP phi2_div
        RTL ${RP6502_SRC}/rtl/cpu65/phi2_div.sv
        TIMEOUT 120)
endif()
