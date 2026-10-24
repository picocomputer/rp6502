rp6502_add_test(fs SOURCES test_fs.c ${RP6502_TB_HOSTOS} LIBS emu_core)

rp6502_add_test(drive SOURCES test_drive.c ${RP6502_TB_HOSTOS}
    LIBS emu_core ROM ${ADVENTURE_ROM})

if(TARGET rp6502-emu)
    rp6502_add_script_test(NAME dir COMMAND dir.py)
    rp6502_add_script_test(NAME drive COMMAND drive.py)
    rp6502_add_script_test(NAME execrel COMMAND execrel.py)
    rp6502_add_script_test(NAME file COMMAND file.py)
    rp6502_add_script_test(NAME fstest COMMAND fstest.py)
    rp6502_add_script_test(NAME install COMMAND install.py)
    rp6502_add_script_test(NAME stream COMMAND stream.py)
endif()
