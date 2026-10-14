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
endif()

if(TARGET rp6502-emu)
    rp6502_add_script_test(psg DRIVER ${RP6502_TESTS_DIR}/gen/aud_rom_gen.py
        ROM ${AUD_ROM_PSG} DEPENDS aud_roms)
    rp6502_add_script_test(psg_pre DRIVER ${RP6502_TESTS_DIR}/gen/aud_rom_gen.py
        ROM ${AUD_ROM_PSG_PRE} ARGS --check pre DEPENDS aud_roms)
    rp6502_add_script_test(opl DRIVER ${RP6502_TESTS_DIR}/gen/aud_rom_gen.py
        ROM ${AUD_ROM_OPL} ARGS --check opl DEPENDS aud_roms)
    rp6502_add_script_test(opl_init DRIVER ${RP6502_TESTS_DIR}/gen/aud_rom_gen.py
        ROM ${AUD_ROM_OPL_INIT} ARGS --check init DEPENDS aud_roms)
    rp6502_add_script_test(opl_exit DRIVER ${RP6502_TESTS_DIR}/gen/aud_rom_gen.py
        ROM ${AUD_ROM_OPL_EXIT} ARGS --check exit DEPENDS aud_roms)
    rp6502_add_script_test(bel DRIVER ${RP6502_TESTS_DIR}/gen/aud_rom_gen.py
        ROM ${AUD_ROM_BEL} ARGS --check bel DEPENDS aud_roms)
    rp6502_add_script_test(opl_bel DRIVER ${RP6502_TESTS_DIR}/gen/aud_rom_gen.py
        ROM ${AUD_ROM_OPL_BEL} ARGS --check opl-bel --base ${AUD_ROM_OPL}
        DEPENDS aud_roms)
    rp6502_add_script_test(bel_off DRIVER ${RP6502_TESTS_DIR}/gen/aud_rom_gen.py
        ROM ${AUD_ROM_BEL_OFF} ARGS --check bel-off DEPENDS aud_roms)
endif()
