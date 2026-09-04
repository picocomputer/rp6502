# PSG, bell, OPL2 and the resampler in the fabric.
#
# psg and core/aud/psg.c are two implementations of one engine and are
# held to the same sample. That is what psg_shim.c exists for — it stands
# the firmware's drivers up over flat shims so the same C runs on a
# workstation.
#
# There is no bell here as a module of its own. The bell is a voice of psg
# that the soft CPU drives, so what used to be its own test is now the
# machine's — tests/cpu/aud, with the rest of what both machines answer.

# host/pocket for "machine.h": these compile core drivers, which ask their
# machine where to place things, and any machine that is not a Pico answers
# the same do-nothing way. The test_apf precedent.
set(AUD_SHIM_INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${RP6502_ASSETS}
    ${RP6502_TEST_TABLES} ${RP6502_SRC}/host/pocket ${RP6502_SRC})

# --- The PSG in RTL, against core/aud/psg.c in lockstep ---
# The bit-exact audio proof: the vendored DSP over flat shims and the
# verilated engine consume the same XRAM image and gate schedule.
rp6502_add_module_test(psg
    TOP psg
    RTL ${AUD_SINE_PKG} ${RP6502_SRC}/core/aud/psg.sv
    SOURCES test_psg.cpp psg_shim.c
        ${RP6502_SRC}/core/aud/psg.c ${RP6502_SRC}/core/aud/bel.c
        ${RP6502_SRC}/core/aud/bel_presets.c
    INCLUDES ${AUD_SHIM_INCLUDES}
    # The C and the model at one rate, which is the Pocket's and not the
    # soft machine's own; the tick is shortened only so the simulation runs
    # faster.
    DEFS AUD_NATIVE_RATE=48000
    DEPENDS aud_sine_rom aud_sine_tables
    ARGS -GRATE=48000 -GTICKS_PER_SAMPLE=1200
    TIMEOUT 300)

# --- The resampler in RTL, against core/aud/rsmp.c in lockstep ---
# test_rsmp says whether the filter is any good; this says whether the
# fabric copies it exactly. Both read one generated coefficient design.
rp6502_add_module_test(rsmp_rtl
    TOP rsmp
    RTL ${RSMP_COEF_PKG} ${RP6502_SRC}/core/aud/rsmp.sv
    SOURCES test_rsmp_rtl.cpp ${RP6502_SRC}/core/aud/rsmp.c
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
    INCLUDES ${RP6502_ASSETS} ${RP6502_TEST_TABLES}
    DEPENDS opl2_lut_rom opl2_lut_tables
    TIMEOUT 120)

# --- The OPL2 device: the pointer gates it, a poke reaches the chip ---
rp6502_add_module_test(opl
    TOP opl
    RTL ${OPL2_SOURCES}
        ${RP6502_SRC}/core/aud/opl.sv
    INCLUDES ${RP6502_BENCH}
    DEPENDS opl2_lut_rom
    TIMEOUT 300)
