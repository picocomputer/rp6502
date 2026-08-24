# The RIA in the fabric: the clock divider, and the machine booting through it.
#
# The staged-table suites are here rather than in tests/emu/ria because only a
# tree that builds the machine's assets has an oemcp.bin or a keyboard.bin to
# be wrong about. They are that tree's own test_uni.c and test_kbl.c over the
# other port, which is exactly the claim: one generator, two readers.

# --- Every whole kilohertz, exactly ---
rp6502_add_module_test(phi2
    TOP phi2_div
    RTL ${RP6502_SRC}/core/ria/phi2_div.sv
    TIMEOUT 120)

# The whole machine so far boots a program that talks through the RIA's
# bare UART.
rp6502_add_machine_test(hello TIMEOUT 120)

if(RP6502_SOFT_CPU)
    # Op 0x01 dispatch against the oracle, and the RTL program it lands.
    rp6502_add_machine_test(xreg ORACLE FIRMWARE TIMEOUT 900)

    # The .rp6502 loader against the staged window, the testbench playing
    # the platform bridge.
    rp6502_add_machine_test(rom FIRMWARE TIMEOUT 900)
endif()

# --- The Pocket's copy of the OEM tables, under the emulator's own suite ---
# uni_word is the one thing a port writes, and there are two ports: one links
# oemcp.c and reads an array, one stages oemcp.bin and reads a window. Both
# come out of one generator and nothing compared them, so uni_bin.c is the
# second port and this is tests/emu/ria's test_uni.c over it.
if(OEMCP_BIN)
    rp6502_add_test(uni_bin
        SOURCES ${RP6502_TESTS_DIR}/emu/ria/test_uni.c
            ${RP6502_SRC}/core/api/uni.c uni_bin.c
            ${RP6502_VENDOR}/fatfs/ffunicode.c
        INCLUDES ${RP6502_SRC} ${RP6502_VENDOR} ${OEMCP_DIR}
        DEFS OEMCP_BIN_PATH="${OEMCP_BIN}")
    add_dependencies(test_uni_bin oemcp oemcp_bin)
    set_source_files_properties(${RP6502_VENDOR}/fatfs/ffunicode.c
        TARGET_DIRECTORY test_uni_bin PROPERTIES COMPILE_DEFINITIONS
        "ff_oem2uni=ref_oem2uni;ff_uni2oem=ref_uni2oem;ff_wtoupper=ref_wtoupper")
endif()

# --- And its copy of the layouts, the same way ---
# kbl_word is the one thing a port writes: one links kbdlay.c and reads an
# array, one stages keyboard.bin and reads a window.
if(KBDLAY_BIN)
    rp6502_add_test(kbl_bin
        SOURCES ${RP6502_TESTS_DIR}/emu/ria/test_kbl.c
            ${RP6502_SRC}/core/hid/kbl.c kbl_bin.c
        INCLUDES ${RP6502_SRC} ${KBDLAY_DIR}
        DEFS KBDLAY_BIN_PATH="${KBDLAY_BIN}")
    add_dependencies(test_kbl_bin kbdlay kbdlay_bin)
endif()
