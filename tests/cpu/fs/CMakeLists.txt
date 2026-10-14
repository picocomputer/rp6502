rp6502_add_test(fs SOURCES test_fs.c ${RP6502_TB_HOSTOS} LIBS emu_core)

rp6502_add_test(drive SOURCES test_drive.c ${RP6502_TB_HOSTOS}
    LIBS emu_core ROM ${ADVENTURE_ROM})

if(TARGET rp6502-emu)
    rp6502_add_script_test(fstest DRIVER ${RP6502_TESTS_DIR}/gen/fstest_rom_gen.py
        ROM ${FSTEST_ROM} DEPENDS fstest_rom TIMEOUT 120)

    rp6502_add_script_test(file DRIVER ${RP6502_TESTS_DIR}/gen/file_rom_gen.py
        ROM ${FILE_ROM} DEPENDS file_rom TIMEOUT 120)
    rp6502_add_script_test(stream DRIVER ${RP6502_TESTS_DIR}/gen/stream_rom_gen.py
        ROM ${STREAM_ROM} DEPENDS stream_rom TIMEOUT 120)

    rp6502_add_script_test(dir DRIVER ${RP6502_TESTS_DIR}/gen/dir_rom_gen.py
        ROM ${DIR_ROM} DEPENDS dir_rom)

    rp6502_add_script_test(install DRIVER install.py ROM ${ADVENTURE_ROM})
endif()
