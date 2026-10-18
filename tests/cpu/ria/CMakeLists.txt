set(ASM_REF ${CMAKE_CURRENT_BINARY_DIR}/asm_ref.bin)
add_custom_command(OUTPUT ${ASM_REF}
    COMMAND ${RP6502_TEST_PYTHON}
        ${CMAKE_CURRENT_LIST_DIR}/asm_ref.py --emit-bin ${ASM_REF}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/asm_ref.py ${RP6502_TEST_LIB_FILES}
    COMMENT "Assembling the reference image"
    VERBATIM)
add_custom_target(asm_ref DEPENDS ${ASM_REF})
rp6502_add_test(asm SOURCES test_asm.cpp DEFS ASM_REF="${ASM_REF}")
add_dependencies(test_asm asm_ref)

add_test(NAME asm_opcodes
    COMMAND ${RP6502_TEST_PYTHON} ${RP6502_TEST_LIB}/rp6502_asm.py --check)
set_tests_properties(asm_opcodes PROPERTIES LABELS "gate;cpu.ria")

add_test(NAME rom_tool
    COMMAND ${RP6502_TEST_PYTHON}
        ${CMAKE_CURRENT_LIST_DIR}/rom_tool_check.py
        --scratch ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rom_tool PROPERTIES LABELS "gate;cpu.ria")

rp6502_add_test(features LIBS emu_core ROM ${ADVENTURE_ROM})

if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(exec FIRMWARE
            SOURCES test_exec.cpp ${RP6502_BENCH}/mut_rtl.cpp
            DEFS EXEC_ROM="${EXEC_ROM}"
            DEPENDS exec_roms
            TIMEOUT 900)
    endif()
elseif(TARGET rp6502_libretro)
    rp6502_add_test(exec LIBS ${CMAKE_DL_LIBS}
        SOURCES test_exec.cpp ${RP6502_BENCH}/mut_libretro.c
        INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
        DEFS EXEC_ROM="${EXEC_ROM}"
            RETRO_SO="$<TARGET_FILE:rp6502_libretro>"
        DEPENDS exec_roms rp6502_libretro
        TIMEOUT 120)
else()
    rp6502_add_test(exec LIBS emu_core
        SOURCES test_exec.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        DEFS EXEC_ROM="${EXEC_ROM}"
        DEPENDS exec_roms
        TIMEOUT 60)
endif()

rp6502_add_test(rtc LIBS emu_core)

if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(time FIRMWARE
            SOURCES test_time.cpp ${RP6502_BENCH}/mut_rtl.cpp
            DEFS TIME_ROM="${STRFTIME_ROM}"
            DEPENDS strftime_roms
            TIMEOUT 900)
    endif()
elseif(TARGET rp6502_libretro)
    rp6502_add_test(time LIBS ${CMAKE_DL_LIBS}
        SOURCES test_time.cpp ${RP6502_BENCH}/mut_libretro.c
        INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
        DEFS TIME_ROM="${STRFTIME_ROM}"
            RETRO_SO="$<TARGET_FILE:rp6502_libretro>"
        DEPENDS strftime_roms rp6502_libretro
        TIMEOUT 120)
else()
    rp6502_add_test(time LIBS emu_core
        SOURCES test_time.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        DEFS TIME_ROM="${STRFTIME_ROM}"
        DEPENDS strftime_roms
        TIMEOUT 60)
endif()

rp6502_add_test(clock LIBS emu_core ROM ${ADVENTURE_ROM})

set_source_files_properties(${OEMCP_C} PROPERTIES GENERATED TRUE)
rp6502_add_test(unicode
    SOURCES test_unicode.c ${RP6502_SRC}/core/str/unicode.c ${OEMCP_C}
        ${RP6502_VENDOR}/fatfs/ffunicode.c
    INCLUDES ${RP6502_SRC}/host/pocket ${RP6502_SRC} ${RP6502_VENDOR}
        ${OEMCP_DIR})
add_dependencies(test_unicode oemcp)
set_source_files_properties(${RP6502_VENDOR}/fatfs/ffunicode.c
    TARGET_DIRECTORY test_unicode PROPERTIES COMPILE_DEFINITIONS
    "ff_oem2uni=ref_oem2uni;ff_uni2oem=ref_uni2oem;ff_wtoupper=ref_wtoupper")

set_source_files_properties(${KBDLAY_C} PROPERTIES GENERATED TRUE)
rp6502_add_test(layout
    SOURCES test_layout.c ${RP6502_SRC}/core/hid/layout.c ${KBDLAY_C}
    INCLUDES ${RP6502_SRC}/host/pocket ${RP6502_SRC} ${KBDLAY_DIR})
add_dependencies(test_layout kbdlay)

if(OEMCP_BIN)
    rp6502_add_test(unicode_bin
        SOURCES test_unicode.c ${RP6502_SRC}/core/str/unicode.c unicode_bin.c
            ${RP6502_VENDOR}/fatfs/ffunicode.c
        INCLUDES ${RP6502_SRC} ${RP6502_VENDOR} ${OEMCP_DIR}
        DEFS OEMCP_BIN_PATH="${OEMCP_BIN}")
    add_dependencies(test_unicode_bin oemcp oemcp_bin)
    set_source_files_properties(${RP6502_VENDOR}/fatfs/ffunicode.c
        TARGET_DIRECTORY test_unicode_bin PROPERTIES COMPILE_DEFINITIONS
        "ff_oem2uni=ref_oem2uni;ff_uni2oem=ref_uni2oem;ff_wtoupper=ref_wtoupper")
endif()

if(KBDLAY_BIN)
    rp6502_add_test(layout_bin
        SOURCES test_layout.c ${RP6502_SRC}/core/hid/layout.c layout_bin.c
        INCLUDES ${RP6502_SRC} ${KBDLAY_DIR}
        DEFS KBDLAY_BIN_PATH="${KBDLAY_BIN}")
    add_dependencies(test_layout_bin kbdlay kbdlay_bin)
endif()

if(COMMAND rp6502_add_machine_test)
    rp6502_add_machine_test(hello TIMEOUT 120)
endif()

if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(xreg FIRMWARE
            SOURCES test_xreg.cpp ${RP6502_BENCH}/mut_rtl.cpp
            TIMEOUT 900)
    endif()
else()
    rp6502_add_test(xreg LIBS emu_core
        SOURCES test_xreg.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        TIMEOUT 60)
endif()

if(RP6502_SOFT_CPU)
    rp6502_add_machine_test(rom FIRMWARE TIMEOUT 900)
endif()

if(TARGET rp6502-emu)
    rp6502_add_script_test(NAME adventure COMMAND adventure.txt ${ADVENTURE_ROM})
    rp6502_add_script_test(NAME args COMMAND argv.py)
    rp6502_add_script_test(NAME argv COMMAND argv.py)
    rp6502_add_script_test(NAME exec_long COMMAND argv.py)
    rp6502_add_script_test(NAME canvas COMMAND canvas.py)
    rp6502_add_script_test(NAME chain COMMAND chain.py)
    rp6502_add_script_test(NAME clk COMMAND clk.py)
    rp6502_add_script_test(NAME eof_empty COMMAND stdio.py)
    rp6502_add_script_test(NAME seed COMMAND seed.py)
    rp6502_add_script_test(NAME seed_range COMMAND seed.py)
    rp6502_add_script_test(NAME leftover COMMAND seed.py)
    rp6502_add_script_test(NAME failure_line COMMAND seed.py)
    rp6502_add_script_test(NAME exit_checks COMMAND seed.py)
    rp6502_add_script_test(NAME discard COMMAND seed.py)
    rp6502_add_script_test(NAME memfill COMMAND seed.py)
    rp6502_add_script_test(NAME status COMMAND seed.py)
    rp6502_add_script_test(NAME load_errors COMMAND seed.py)
    rp6502_add_script_test(NAME sigint COMMAND sigint.py)
    rp6502_add_script_test(NAME stdio COMMAND stdio.py)
    rp6502_add_script_test(NAME close_console COMMAND stdio.py)
    rp6502_add_script_test(NAME tty_keys COMMAND tty_keys.py)
    rp6502_add_script_test(NAME tty_pipe COMMAND tty_pipe.py)
    rp6502_add_script_test(NAME zeroread COMMAND zeroread.py)
endif()
