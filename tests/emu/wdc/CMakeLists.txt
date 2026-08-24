# The 65C02 and its VIA, as the emulator implements them.
#
# The evidence lives here and is read from both trees: dut.h is the interface,
# vec.c and klaus.c are the corpora, chips_dut.c is this machine's
# implementation under them. tests/rtl/wdc compiles these same files against
# w65c02_dut.cpp, which is the fabric's — so the two are held to one standard
# rather than to two copies of one.

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
