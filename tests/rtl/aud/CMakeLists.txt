set(AUD_SHIM_INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${RP6502_ASSETS}
    ${AUD_SINE_DIR} ${RP6502_SRC}/host/pocket ${RP6502_SRC})

rp6502_add_module_test(psg
    TOP psg
    RTL ${AUD_SINE_PKG} ${RP6502_SRC}/core/aud/psg.sv
    SOURCES test_psg.cpp psg_shim.c
        ${RP6502_SRC}/core/aud/psg.c
    INCLUDES ${AUD_SHIM_INCLUDES}
    DEFS AUD_NATIVE_RATE=48000
    DEPENDS aud_sine_rom aud_sine
    ARGS -GRATE=48000 -GTICKS_PER_SAMPLE=1200
    TIMEOUT 300)

# pcm, the OPL2's resampler and PCM on one filter, as wiring.sv has it.
set(AUD_BENCH_RTL ${RSMP_COEF_PKG}
    ${RP6502_SRC}/core/aud/pcm.sv ${CMAKE_CURRENT_LIST_DIR}/tb_aud.sv)

rp6502_add_module_test(rsmp_rtl
    TOP tb_aud
    RTL ${AUD_BENCH_RTL}
    SOURCES test_rsmp_rtl.cpp ${RP6502_SRC}/core/aud/rsmp.c
    INCLUDES ${RP6502_SRC} ${RSMP_COEF_DIR}
    DEPENDS rsmp_coef rsmp_coef_pkg
    ARGS -GSST_CLK=0
    TIMEOUT 300)

rp6502_add_module_test(pcm_rtl
    TOP tb_aud
    RTL ${AUD_BENCH_RTL}
    SOURCES test_pcm_rtl.cpp pcm_shim.c ${RP6502_SRC}/core/aud/rsmp.c
    INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${RP6502_SRC} ${RSMP_COEF_DIR}
    DEFS PICO_PROGRAM_NAME="RP6502-EMU" RP6502_EXFAT=0 AUD_NATIVE_RATE=48000
    DEPENDS rsmp_coef rsmp_coef_pkg
    ARGS -GSST_CLK=0
    TIMEOUT 600)

# YM3812-LLE, nukeykt's die model, is the oracle opl.sv is held to, as opl.c
# is. It is GPL-2 and is compiled into this test only.
rp6502_submodule(vendor/YM3812-LLE SENTINEL fmopl2.c
    WANTS "the OPL2 test" OPTIONAL RESULT RP6502_HAVE_LLE)
if(RP6502_HAVE_LLE)
    set(_emu ${RP6502_ROOT}/tests/host/emu)
    set(_lle ${RP6502_VENDOR}/YM3812-LLE)
    rp6502_add_module_test(opl SPLIT
        TOP opl
        RTL ${OPL_ROM_PKG} ${RP6502_SRC}/core/aud/opl.sv
        SOURCES test_opl.cpp ${RP6502_SRC}/core/aud/opl.c
            ${_emu}/opl_script.c ${_emu}/opl_lle.c ${_lle}/fmopl2.c
        INCLUDES ${OPL_ROM_DIR} ${_emu} ${_lle}
        DEPENDS opl_rom opl_rom_pkg
        ARGS -GSST_CLK=0
        TIMEOUT 600)
    set_source_files_properties(${_lle}/fmopl2.c PROPERTIES
        COMPILE_OPTIONS "-O2;-w")
else()
    message(WARNING "YM3812-LLE absent -- the OPL2 RTL test skipped.")
endif()

# The savestate port of each engine: two sessions, one restored from the
# other's image, held to each other sample for sample.
rp6502_add_module_test(opl_sst SPLIT
    TOP opl
    PREFIX Vopl_sst
    RTL ${OPL_ROM_PKG} ${RP6502_SRC}/core/aud/opl.sv
    DEPENDS opl_rom_pkg
    TIMEOUT 600)

rp6502_add_module_test(psg_sst SPLIT
    TOP psg
    PREFIX Vpsg_sst
    RTL ${AUD_SINE_PKG} ${RP6502_SRC}/core/aud/psg.sv
    DEPENDS aud_sine_rom
    TIMEOUT 600)

rp6502_add_module_test(aud_sst SPLIT
    TOP tb_aud
    PREFIX Vtb_aud_sst
    RTL ${AUD_BENCH_RTL}
    DEPENDS rsmp_coef_pkg
    TIMEOUT 600)
