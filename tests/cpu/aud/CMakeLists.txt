# The audio path the machine owns: the resampler's arithmetic, and both
# engines end to end.
#
# The PSG and the OPL2 as modules are the fabric's own — a bit-exact lockstep
# and a LUT ROM have no meaning without one — so those live in tests/rtl/aud.

# --- The resampler, on its own: no emulator, no 6502, just arithmetic and a
# ruler. Measures the filter rather than asserting it, so a change that
# quietly makes it worse fails here instead of on someone's headphones. ---
rp6502_add_test(rsmp
    SOURCES test_rsmp.c ${RP6502_SRC}/core/aud/rsmp.c
    INCLUDES ${RP6502_SRC} ${RSMP_COEF_DIR})
add_dependencies(test_rsmp rsmp_coef)
if(NOT WIN32)
    target_link_libraries(test_rsmp PRIVATE m)
endif()

if(RP6502_SOFT_CPU)
    # Both engines end to end: a program asks for the device, pours its
    # registers in through RW0, and the machine makes a noise inside a couple
    # of frames. This is the plumbing test — the xreg dispatch, the write
    # snoop, and the sample grid — not a waveform one; test_psg holds the PSG
    # bit-exact, and the OPL2 has no emulator to agree with.
    rp6502_add_machine_test(aud FIRMWARE
        DEFS AUD_ROM_PSG="${AUD_ROM_PSG}" AUD_ROM_OPL="${AUD_ROM_OPL}"
            AUD_ROM_PSG_PRE="${AUD_ROM_PSG_PRE}"
            AUD_ROM_OPL_EXIT="${AUD_ROM_OPL_EXIT}"
            AUD_ROM_BEL="${AUD_ROM_BEL}" AUD_ROM_OPL_BEL="${AUD_ROM_OPL_BEL}"
        DEPENDS aud_roms
        TIMEOUT 600)
endif()
