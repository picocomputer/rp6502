# The resampler and the pump, on this machine.
#
# The PSG, the OPL2 and the bell are held to the fabric rather than to a
# recording, so their suites are the RTL tree's — tests/rtl/aud, where the
# same core drivers are compiled against the verilated engines.

# --- The resampler, on its own: no emulator, no 6502, just arithmetic and a
# ruler. Measures the filter rather than asserting it, so a change that
# quietly makes it worse fails here instead of on someone's headphones. ---
rp6502_add_test(rsmp
    SOURCES test_rsmp.c ${RP6502_SRC}/core/emu/emu/rsmp.c
    INCLUDES ${RP6502_SRC} ${RSMP_COEF_DIR})
add_dependencies(test_rsmp rsmp_coef)
if(NOT WIN32)
    target_link_libraries(test_rsmp PRIVATE m)
endif()

# --- aud_pump: the seam between the machine's rate and the host's ---
rp6502_add_test(pump LIBS emu_core TIMEOUT 60)
