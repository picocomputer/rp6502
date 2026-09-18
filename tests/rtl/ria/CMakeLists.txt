rp6502_add_machine_test(xregs FIRMWARE
    SOURCES test_xregs.cpp
    INCLUDES ${RP6502_TESTS_DIR}/cpu/ria
    TIMEOUT 900)
