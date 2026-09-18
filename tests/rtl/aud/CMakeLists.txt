set(AUD_SHIM_INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${RP6502_ASSETS}
    ${RP6502_TEST_TABLES} ${RP6502_SRC}/host/pocket ${RP6502_SRC})

rp6502_add_module_test(psg
    TOP psg
    RTL ${AUD_SINE_PKG} ${RP6502_SRC}/core/aud/psg.sv
    SOURCES test_psg.cpp psg_shim.c
        ${RP6502_SRC}/core/aud/psg.c ${RP6502_SRC}/core/aud/bel.c
        ${RP6502_SRC}/core/aud/bel_presets.c
    INCLUDES ${AUD_SHIM_INCLUDES}
    DEFS AUD_NATIVE_RATE=48000
    DEPENDS aud_sine_rom aud_sine_tables
    ARGS -GRATE=48000 -GTICKS_PER_SAMPLE=1200
    TIMEOUT 300)

rp6502_add_module_test(rsmp_rtl
    TOP rsmp
    RTL ${RSMP_COEF_PKG} ${RP6502_SRC}/core/aud/rsmp.sv
    SOURCES test_rsmp_rtl.cpp ${RP6502_SRC}/core/aud/rsmp.c
    INCLUDES ${RP6502_SRC} ${RSMP_COEF_DIR}
    DEPENDS rsmp_coef rsmp_coef_pkg
    TIMEOUT 300)

rp6502_add_module_test(oplrom
    TOP opl2_lut_rom
    RTL ${OPL2_LUT_PKG}
        ${RP6502_VENDOR}/opl2_fpga_rp6502/opl2_lut_rom.sv
    INCLUDES ${RP6502_ASSETS} ${RP6502_TEST_TABLES}
    DEPENDS opl2_lut_rom opl2_lut_tables
    TIMEOUT 120)

rp6502_add_module_test(opl
    TOP opl
    RTL ${OPL2_SOURCES}
        ${RP6502_SRC}/core/aud/opl.sv
    INCLUDES ${RP6502_BENCH}
    DEPENDS opl2_lut_rom
    TIMEOUT 300)
