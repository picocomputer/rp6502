# The RIA in the fabric: what the divider does between whole kilohertz.
#
# The machine booting through it, the xreg dispatch and the loader are
# machine claims and live in tests/cpu/ria.

# --- Every whole kilohertz, exactly ---
rp6502_add_module_test(phi2
    TOP phi2_div
    RTL ${RP6502_SRC}/core/ria/phi2_div.sv
    TIMEOUT 120)

# The xreg program's landing site. It boots the probe program from
# tests/cpu/ria, where the console those probes print is checked on both
# machines; what is left over is the registers only this machine has.
rp6502_add_machine_test(xregs FIRMWARE
    SOURCES test_xregs.cpp
    INCLUDES ${RP6502_TESTS_DIR}/cpu/ria
    TIMEOUT 900)
