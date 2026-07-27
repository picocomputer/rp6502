include(${CMAKE_CURRENT_LIST_DIR}/../rp6502_tests.cmake)

# SingleStepTests vectors are a developer download (see README), not a
# submodule: upstream carries five CPU families and checks out at 4.8 GB.
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

    rp6502_add_test(w65c02
        SOURCES test_w65c02.c vec.c
        INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${RP6502_VENDOR}/chips_rp6502 ${RP6502_VENDOR}
        DEFS VECTORS="${VECTORS}"
        TIMEOUT 300)
    add_dependencies(test_w65c02 cpu_vectors)
else()
    message(WARNING
        "SingleStepTests vectors absent — CPU conformance tests skipped.\n"
        "  python3 tests/cpu/vectors.py")
endif()
