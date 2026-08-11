# PSG, bell, OPL2 and the resampler.
#
# aud_psg and ria/aud/psg.c are two implementations of one engine and are
# held to the same sample. That is what psg_shim.c exists for — it stands
# the firmware's drivers up over flat shims so the same C runs on a
# workstation.
#
# There is no bell here any more. The bell is a voice of aud_psg that the
# soft CPU drives, so what used to be its own module test is now the
# machine's — see test_aud.

# --- The resampler, on its own: no emulator, no 6502, just arithmetic and a
# ruler. Measures the filter rather than asserting it, so a change that
# quietly makes it worse fails here instead of on someone's headphones. ---
rp6502_add_test(rsmp
    SOURCES test_rsmp.c ${RP6502_SRC}/emu/emu/rsmp.c
    INCLUDES ${RP6502_SRC} ${RSMP_COEF_DIR})
add_dependencies(test_rsmp rsmp_coef)
if(NOT WIN32)
    target_link_libraries(test_rsmp PRIVATE m)
endif()

# --- aud_pump: the seam between the machine's rate and the host's ---
rp6502_add_test(pump LIBS emu_core TIMEOUT 60)

if(RP6502_VERILATE)
    # Whether the RISC-V's writes are on the bus the audio engines
    # watch, which is the claim every restore of a playing machine
    # rests on and which nothing tested.
    rp6502_add_machine_test(replay TIMEOUT 300)

    set(AUD_SHIM_INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${RP6502_ASSETS}
        ${RP6502_SRC} ${RP6502_SRC}/host/pico)

    # --- The PSG in RTL, against ria/aud/psg.c in lockstep ---
    # The bit-exact audio proof: the vendored DSP over flat shims and the
    # verilated engine consume the same XRAM image and gate schedule.
    rp6502_add_module_test(psg
        TOP aud_psg
        RTL ${AUD_SINE_PKG} ${RP6502_SRC}/rtl/aud/aud_psg.sv
        SOURCES test_psg.cpp psg_shim.c
            ${RP6502_SRC}/ria/aud/psg.c ${RP6502_SRC}/ria/aud/bel.c
            ${RP6502_SRC}/ria/aud/bel_presets.c
        INCLUDES ${AUD_SHIM_INCLUDES}
        DEPENDS aud_sine_rom
        # RATE is the arithmetic and must match PSG_SHIM_RATE; the tick is
        # shortened only so the simulation runs faster.
        ARGS -GRATE=48000 -GTICKS_PER_SAMPLE=1200
        TIMEOUT 300)

    # --- The resampler in RTL, against emu/emu/rsmp.c in lockstep ---
    # test_rsmp says whether the filter is any good; this says whether the
    # fabric copies it exactly. Both read one generated coefficient design.
    rp6502_add_module_test(rsmp_rtl
        TOP aud_rsmp
        RTL ${RSMP_COEF_PKG} ${RP6502_SRC}/rtl/aud/aud_rsmp.sv
        SOURCES test_rsmp_rtl.cpp ${RP6502_SRC}/emu/emu/rsmp.c
        INCLUDES ${RP6502_SRC} ${RSMP_COEF_DIR}
        DEPENDS rsmp_coef rsmp_coef_pkg
        TIMEOUT 300)

    # --- The merged OPL2 LUT ROM against the generated tables ---
    # The generator holds the words to the vendor's case arms and their
    # documented formulas; this holds the module to the words.
    rp6502_add_module_test(oplrom
        TOP opl2_lut_rom
        RTL ${OPL2_LUT_PKG}
            ${RP6502_VENDOR}/opl2_fpga_rp6502/opl2_lut_rom.sv
        INCLUDES ${RP6502_ASSETS}
        DEPENDS opl2_lut_rom
        TIMEOUT 120)

    # --- The OPL2 device: the pointer gates it, a poke reaches the chip ---
    rp6502_add_module_test(opl
        TOP aud_opl
        RTL ${OPL2_SOURCES}
            ${RP6502_SRC}/rtl/aud/aud_opl.sv
        INCLUDES ${RP6502_BENCH}
        DEPENDS opl2_lut_rom
        TIMEOUT 300)
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
