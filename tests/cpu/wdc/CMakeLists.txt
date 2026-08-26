# The 65C02 and its VIA.
#
# Each exists twice — a C model in the emulator, RTL in the fabric — and each
# has an interface both implementations answer: dut.h for the CPU, via_dut.h
# for the VIA. So the suites here are written once and bound to whichever
# machine this tree built, and the corpora, the scenarios and the recorded
# traces are held by one copy each. Neither implementation has a standard of
# its own, and neither can drift alone.

set(CPU_INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${RP6502_VENDOR})

# rp6502_add_cpu_test(<name> SOURCES ... [DEFS ...])
# rp6502_add_via_test(<name> SOURCES ... [DEFS ...])
#
# One suite, bound to whichever implementation this tree built: the C model
# where there is no simulator, the verilated module where there is. The
# timeout goes with the binding rather than the suite, because what a corpus
# costs is a fact about the machine running it and not about the claim.
function(rp6502_add_cpu_test name)
    cmake_parse_arguments(P "" "" "SOURCES;DEFS" ${ARGN})
    if(COMMAND rp6502_add_module_test)
        rp6502_add_module_test(${name}
            TOP w65c02
            RTL ${W65C02_ROM} ${RP6502_SRC}/core/wdc/w65c02.sv
            SOURCES ${P_SOURCES} cpu_dut_rtl.cpp w65c02_dut.cpp
            INCLUDES ${CPU_INCLUDES}
            DEFS ${P_DEFS}
            DEPENDS w65c02_rom
            TIMEOUT 900)
    else()
        rp6502_add_test(${name}
            SOURCES ${P_SOURCES} cpu_dut_chips.c chips_dut.c
            INCLUDES ${CPU_INCLUDES}
            DEFS ${P_DEFS}
            TIMEOUT 300)
    endif()
endfunction()

function(rp6502_add_via_test name)
    cmake_parse_arguments(P "" "" "SOURCES;DEFS" ${ARGN})
    if(COMMAND rp6502_add_module_test)
        rp6502_add_module_test(${name}
            TOP w65c22
            RTL ${RP6502_SRC}/core/wdc/w65c22.sv
            SOURCES ${P_SOURCES} via_rtl.cpp
            INCLUDES ${CPU_INCLUDES}
            DEFS ${P_DEFS}
            TIMEOUT 300)
    else()
        rp6502_add_test(${name}
            SOURCES ${P_SOURCES} via_chips.c
            INCLUDES ${CPU_INCLUDES}
            DEFS ${P_DEFS}
            TIMEOUT 300)
    endif()
endfunction()

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

    rp6502_add_cpu_test(vectors
        SOURCES test_vectors.c vec.c
        DEFS VECTORS="${VECTORS}")
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
    rp6502_add_cpu_test(klaus
        SOURCES test_klaus.c klaus.c
        DEFS
            KLAUS_6502="${KLAUS}/6502_functional_test.bin"
            KLAUS_65C02="${KLAUS}/65C02_extended_opcodes_test.bin")
else()
    message(WARNING "Klaus Dormann images absent — functional soak skipped.")
endif()

# --- The CPU against its own recorded trace ---
# The corpora above say the CPU is right; this says it has not changed, which
# is a weaker claim that survives a checkout without them. It also covers the
# pins, which neither corpus exercises. The emulator's model only: the trace
# is of chips_dut's pins, and the fabric's are the lockstep test's business.
rp6502_add_test(w65c02_chips
    SOURCES test_w65c02_chips.c lockstep_scen.c chips_dut.c
    INCLUDES ${CPU_INCLUDES}
    DEFS W65C02_GOLDEN="${CMAKE_CURRENT_LIST_DIR}/w65c02_golden.txt")

# --- The VIA, directed cases and recorded traces ---
# T1/T2 timers and their interrupts are live machine features; see
# core/wdc/via.c for the wiring both implementations are driven through.
rp6502_add_via_test(w65c22
    SOURCES test_w65c22.c w65c22_scen.c
    DEFS W65C22_GOLDEN="${CMAKE_CURRENT_LIST_DIR}/w65c22_golden.txt")
