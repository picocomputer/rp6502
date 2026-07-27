include(${CMAKE_CURRENT_LIST_DIR}/../rp6502_tests.cmake)

# The emulator's 65C02 and the FPGA core's answer the same dut.h, so both are
# held to these suites. Each suite is guarded on its own fixtures: the Klaus
# images arrive with the submodules, the SingleStepTests vectors are a
# developer download (see README).
set(CPU_DUT ${CMAKE_CURRENT_LIST_DIR}/chips_dut.c)
set(CPU_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}
    ${RP6502_VENDOR}/chips_rp6502 # shadows vendor/chips; must precede it
    ${RP6502_VENDOR})

# --- SingleStepTests: per-cycle bus traces, one instruction per case ---
set(SSTESTS ${RP6502_ROOT}/vendor/65x02/wdc65c02/v1)
set(SSVEC_COUNT 100 CACHE STRING "SingleStepTests cases per opcode")

if(EXISTS ${SSTESTS})
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
    message(WARNING
        "SingleStepTests vectors absent — per-cycle conformance skipped.\n"
        "  python3 tests/cpu/vectors.py")
endif()

# --- Klaus Dormann: long self-checking sequences ---
# GPL-3.0, so it stays a submodule reference rather than bytes in this BSD-3
# tree. The images are data the tests read; nothing is linked or shipped.
set(KLAUS ${RP6502_ROOT}/vendor/6502_65C02_functional_tests/bin_files)

if(EXISTS ${KLAUS}/6502_functional_test.bin)
    rp6502_add_test(klaus
        SOURCES test_klaus.c klaus.c ${CPU_DUT}
        INCLUDES ${CPU_INCLUDES}
        DEFS
            KLAUS_6502="${KLAUS}/6502_functional_test.bin"
            KLAUS_65C02="${KLAUS}/65C02_extended_opcodes_test.bin"
        TIMEOUT 300)
else()
    message(WARNING
        "Klaus Dormann images absent — functional soak skipped.\n"
        "  git submodule update --init vendor/6502_65C02_functional_tests")
endif()
