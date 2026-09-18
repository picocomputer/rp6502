rp6502_add_test(rsmp
    SOURCES test_rsmp.c ${RP6502_SRC}/core/aud/rsmp.c
    INCLUDES ${RP6502_SRC} ${RSMP_COEF_DIR})
add_dependencies(test_rsmp rsmp_coef)
if(NOT WIN32)
    target_link_libraries(test_rsmp PRIVATE m)
endif()

if(RP6502_SOFT_CPU)
    rp6502_add_machine_test(aud FIRMWARE
        DEFS AUD_ROM_PSG="${AUD_ROM_PSG}" AUD_ROM_OPL="${AUD_ROM_OPL}"
            AUD_ROM_PSG_PRE="${AUD_ROM_PSG_PRE}"
            AUD_ROM_OPL_EXIT="${AUD_ROM_OPL_EXIT}"
            AUD_ROM_BEL="${AUD_ROM_BEL}" AUD_ROM_OPL_BEL="${AUD_ROM_OPL_BEL}"
        DEPENDS aud_roms
        TIMEOUT 600)
else()
    rp6502_add_test(aud LIBS emu_core
        DEFS AUD_ROM_PSG="${AUD_ROM_PSG}" AUD_ROM_OPL="${AUD_ROM_OPL}"
            AUD_ROM_PSG_PRE="${AUD_ROM_PSG_PRE}"
            AUD_ROM_OPL_EXIT="${AUD_ROM_OPL_EXIT}"
            AUD_ROM_OPL_INIT="${AUD_ROM_OPL_INIT}"
            AUD_ROM_BEL="${AUD_ROM_BEL}" AUD_ROM_OPL_BEL="${AUD_ROM_OPL_BEL}"
        DEPENDS aud_roms)
endif()
