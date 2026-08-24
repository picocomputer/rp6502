# The 65C02 and its VIA.
#
# The 6502 exists twice, and dut.h is what both answer: chips_dut.c is the
# emulator's, w65c02_dut.cpp the fabric's. The corpora and the runners — vec.c,
# klaus.c, the scenarios, the recorded traces — are the evidence, held by one
# copy each, so neither implementation can drift alone and neither has a
# standard of its own.

set(CPU_INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${RP6502_VENDOR})

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
    # The encoder decides what the corpus is, so a change to it is a change to
    # what both CPUs are held to.
    set_property(GLOBAL APPEND PROPERTY RP6502_TEST_INPUTS
        ${CMAKE_CURRENT_LIST_DIR}/ssvec.py)

    rp6502_add_test(vectors
        SOURCES test_vectors.c vec.c chips_dut.c
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
        SOURCES test_klaus.c klaus.c chips_dut.c
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
    SOURCES test_w65c02_chips.c lockstep_scen.c chips_dut.c
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

# The same corpora and the same scenarios, against the fabric's CPU and VIA.
# Only a tree with a simulator can register these; the sources beside them are
# the ones above.
if(COMMAND rp6502_add_module_test)
    if(RP6502_HAVE_SSTESTS AND RP6502_HAVE_KLAUS)
        rp6502_add_module_test(w65c02
            TOP w65c02
            RTL ${W65C02_ROM} ${RP6502_SRC}/core/wdc/w65c02.sv
            SOURCES test_w65c02.cpp w65c02_dut.cpp vec.c klaus.c
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

    # --- The VIA in RTL, against chips/m6522.h in lockstep ---
    # T1/T2 timers and their interrupts are live machine features; see
    # emu/emu/via.c for the wiring this reproduces.
    rp6502_add_module_test(w65c22
        TOP w65c22
        RTL ${RP6502_SRC}/core/wdc/w65c22.sv
        SOURCES test_w65c22.cpp w65c22_scen.c
        INCLUDES ${CPU_INCLUDES}
        TIMEOUT 300)
endif()
