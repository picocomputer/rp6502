rp6502_add_test(rsmp
    SOURCES test_rsmp.c ${RP6502_SRC}/core/aud/rsmp.c
    INCLUDES ${RP6502_SRC} ${RSMP_COEF_DIR})
add_dependencies(test_rsmp rsmp_coef)
if(NOT WIN32)
    target_link_libraries(test_rsmp PRIVATE m)
endif()

rp6502_add_test(pcm
    SOURCES test_pcm.c ${RP6502_SRC}/core/aud/pcm.c ${RP6502_SRC}/core/aud/rsmp.c
    INCLUDES ${RP6502_SRC} ${RSMP_COEF_DIR}
    DEFS PICO_PROGRAM_NAME="RP6502-EMU" RP6502_EXFAT=0)
add_dependencies(test_pcm rsmp_coef)

# At the 48 kHz the Pocket plays at, where 48000 Hz is a step of one.
rp6502_add_test(pcm48
    SOURCES test_pcm.c ${RP6502_SRC}/core/aud/pcm.c ${RP6502_SRC}/core/aud/rsmp.c
    INCLUDES ${RP6502_SRC} ${RSMP_COEF_DIR}
    DEFS PICO_PROGRAM_NAME="RP6502-EMU" RP6502_EXFAT=0 AUD_NATIVE_RATE=48000)
add_dependencies(test_pcm48 rsmp_coef)

if(RP6502_SOFT_CPU)
    rp6502_add_machine_test(aud FIRMWARE
        DEFS AUD_ROM_PSG="${PSG_ROM}" AUD_ROM_OPL="${OPL_ROM}"
            AUD_ROM_PSG_PRE="${PSG_PRE_ROM}"
            AUD_ROM_OPL_EXIT="${OPL_EXIT_ROM}"
            AUD_ROM_OPL_INIT="${OPL_INIT_ROM}"
            AUD_ROM_OPL_OVERRUNS="${OPL_OVERRUNS_ROM}"
            AUD_ROM_BEL="${BEL_ROM}" AUD_ROM_OPL_BEL="${OPL_BEL_ROM}"
            AUD_ROM_PCM="${PCM_ROM}" AUD_ROM_PCM_RULES="${PCM_RULES_ROM}"
            AUD_ROM_PCM_ORDER="${PCM_ORDER_ROM}"
            AUD_ROM_PCM_DRY="${PCM_DRY_ROM}"
        DEPENDS aud_roms
        TIMEOUT 600)
endif()

if(TARGET rp6502-emu)
    foreach(test psg psg_pre opl opl_init opl_exit bel opl_bel bel_off pcm
            pcm_rules)
        rp6502_add_script_test(NAME ${test} COMMAND aud.py)
    endforeach()
endif()
