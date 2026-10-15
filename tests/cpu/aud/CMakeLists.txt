rp6502_add_test(rsmp
    SOURCES test_rsmp.c ${RP6502_SRC}/core/aud/rsmp.c
    INCLUDES ${RP6502_SRC} ${RSMP_COEF_DIR})
add_dependencies(test_rsmp rsmp_coef)
if(NOT WIN32)
    target_link_libraries(test_rsmp PRIVATE m)
endif()

if(RP6502_SOFT_CPU)
    rp6502_add_machine_test(aud FIRMWARE
        DEFS AUD_ROM_PSG="${PSG_ROM}" AUD_ROM_OPL="${OPL_ROM}"
            AUD_ROM_PSG_PRE="${PSG_PRE_ROM}"
            AUD_ROM_OPL_EXIT="${OPL_EXIT_ROM}"
            AUD_ROM_BEL="${BEL_ROM}" AUD_ROM_OPL_BEL="${OPL_BEL_ROM}"
        DEPENDS aud_roms
        TIMEOUT 600)
endif()

if(TARGET rp6502-emu)
    foreach(test psg psg_pre opl opl_init opl_exit bel opl_bel bel_off)
        rp6502_add_script_test(NAME ${test} COMMAND aud.py)
    endforeach()
endif()
