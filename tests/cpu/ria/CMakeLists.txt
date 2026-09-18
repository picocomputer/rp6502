set(ASM_REF ${CMAKE_CURRENT_BINARY_DIR}/asm_ref.bin)
add_custom_command(OUTPUT ${ASM_REF}
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${CMAKE_CURRENT_LIST_DIR}/asm_ref.py --emit-bin ${ASM_REF}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/asm_ref.py
        ${RP6502_TESTS_DIR}/gen/rp6502_asm.py
        ${RP6502_TESTS_DIR}/gen/rp6502_rom.py
    COMMENT "Assembling the reference image"
    VERBATIM)
add_custom_target(asm_ref DEPENDS ${ASM_REF})
rp6502_add_test(asm SOURCES test_asm.cpp DEFS ASM_REF="${ASM_REF}")
add_dependencies(test_asm asm_ref)

add_test(NAME asm_opcodes
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_TESTS_DIR}/gen/rp6502_asm.py --check)
set_tests_properties(asm_opcodes PROPERTIES LABELS "gate;cpu.ria")

add_test(NAME rom_tool
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${CMAKE_CURRENT_LIST_DIR}/rom_tool_check.py
        --scratch ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rom_tool PROPERTIES LABELS "gate;cpu.ria")

rp6502_add_test(features LIBS emu_core ROM ${ADVENTURE_ROM}
    DEFS EXEC_ROM="${EXEC_ROM}" ROMS_DIR="${RP6502_TEST_CORPUS}"
    DEPENDS exec_rom)

rp6502_add_test(adventure LIBS emu_core ROM ${ADVENTURE_ROM} TIMEOUT 60)

if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(exec FIRMWARE
            SOURCES test_exec.cpp ${RP6502_BENCH}/mut_rtl.cpp
            DEFS EXEC_ROM="${EXEC_ROM}"
            DEPENDS exec_rom
            TIMEOUT 900)
    endif()
elseif(TARGET rp6502_libretro)
    rp6502_add_test(exec LIBS ${CMAKE_DL_LIBS}
        SOURCES test_exec.cpp ${RP6502_BENCH}/mut_libretro.c
        INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
        DEFS EXEC_ROM="${EXEC_ROM}"
            RETRO_SO="$<TARGET_FILE:rp6502_libretro>"
        DEPENDS exec_rom rp6502_libretro
        TIMEOUT 120)
else()
    rp6502_add_test(exec LIBS emu_core
        SOURCES test_exec.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        DEFS EXEC_ROM="${EXEC_ROM}"
        DEPENDS exec_rom
        TIMEOUT 60)
endif()

rp6502_add_test(rtc SOURCES test_rtc.c ${RP6502_TB_HOSTOS}
    LIBS emu_core DEFS ROMS_DIR="${RP6502_TEST_CORPUS}" TIMEOUT 60)

if(COMMAND rp6502_add_machine_test)
    if(RP6502_SOFT_CPU)
        rp6502_add_machine_test(time FIRMWARE
            SOURCES test_time.cpp ${RP6502_BENCH}/mut_rtl.cpp
            DEFS TIME_ROM="${TIME_ROM}"
            DEPENDS time_rom
            TIMEOUT 900)
    endif()
elseif(TARGET rp6502_libretro)
    rp6502_add_test(time LIBS ${CMAKE_DL_LIBS}
        SOURCES test_time.cpp ${RP6502_BENCH}/mut_libretro.c
        INCLUDES ${RP6502_BENCH} ${RP6502_LIBRETRO_INCLUDE}
        DEFS TIME_ROM="${TIME_ROM}"
            RETRO_SO="$<TARGET_FILE:rp6502_libretro>"
        DEPENDS time_rom rp6502_libretro
        TIMEOUT 120)
else()
    rp6502_add_test(time LIBS emu_core
        SOURCES test_time.cpp ${RP6502_BENCH}/mut_emu.c
        INCLUDES ${RP6502_BENCH}
        DEFS TIME_ROM="${TIME_ROM}"
        DEPENDS time_rom
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
    rp6502_add_script_test(tty DRIVER ${RP6502_TESTS_DIR}/gen/tty_rom_gen.py
        ROM ${TTY_ROM} DEPENDS tty_rom TIMEOUT 120)

    rp6502_add_script_test(stdio DRIVER ${RP6502_TESTS_DIR}/gen/stdio_rom_gen.py
        ROM ${STDIO_ROM} DEPENDS stdio_rom TIMEOUT 120)

    rp6502_add_script_test(con DRIVER ${RP6502_TESTS_DIR}/gen/con_rom_gen.py
        ROM ${CON_ROM} DEPENDS con_rom TIMEOUT 120)

    rp6502_add_script_test(adventure ROM ${ADVENTURE_ROM})
endif()
