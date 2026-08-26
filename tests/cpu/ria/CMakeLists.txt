# The RIA: the xreg dispatch, the API surface, the ROM loader and the clock.

# --- The two assemblers agree where they overlap ---
# A program is built by a generator writing a file and by a bench
# building one in memory, so the encoding has a Python spelling and a
# C++ one. asm_ref.py writes the reference; test_asm.cpp writes the same
# program the other way and compares the assembled bytes. The container
# is not part of the claim: tools/rp6502.py is the only writer of it now,
# and tb_rom.h writes the headerless form on purpose. No emulator and no
# simulator, so it runs wherever the suite does.
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

# The Python assembler reads its opcodes out of vendor/chips rather than
# carrying its own, so a bump that renames or reshuffles that table would
# otherwise be found by a ROM that still assembles and no longer runs.
add_test(NAME asm_opcodes
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_TESTS_DIR}/gen/rp6502_asm.py --check)
set_tests_properties(asm_opcodes PROPERTIES LABELS "gate;ria")

# The container, against the tool that defines it. Every generated ROM is
# packaged by tools/rp6502.py now, so what it writes and what it reads have
# to be the same format — and its chunks have to fit the buffer the monitor
# reads them into, which is the difference between a ROM that runs
# everywhere and one that runs everywhere except hardware.
add_test(NAME rom_tool
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${CMAKE_CURRENT_LIST_DIR}/rom_tool_check.py
        --scratch ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(rom_tool PROPERTIES LABELS "gate;ria")

# --- Feature interfaces ($FFF0 SIGINT IRQ, launcher chain, teletype bell) ---
rp6502_add_test(features LIBS emu_core FIXTURE adventure.rp6502)

# --- Adventure integration (ROM-asset files + interactive stdin via rln) ---
rp6502_add_test(adventure LIBS emu_core FIXTURE adventure.rp6502 TIMEOUT 60)

# --- Exec/argv (proc.c ops 0x08/0x09) + MSC0: program loading ---
rp6502_add_test(exec LIBS emu_core FIXTURE exec.rp6502 TIMEOUT 60)

# --- Clock/time API (host wall clock + timezone + locale + OEM code page) ---
rp6502_add_test(rtc SOURCES test_rtc.c ${RP6502_TB_HOSTOS}
    LIBS emu_core FIXTURE rtc.rp6502 TIMEOUT 60)

# --- Master clock + PHI2 in the emulator (reproducible run timing) ---
rp6502_add_test(clock LIBS emu_core FIXTURE adventure.rp6502)

# --- OEM code page conversion (core/api/uni.c) against the ffunicode.c it
# replaces. The vendored file is compiled beside it with its entry points
# renamed, so every code page, every byte, every code point in the BMP,
# and the whole up-case map are compared in one process. ---
set_source_files_properties(${OEMCP_C} PROPERTIES GENERATED TRUE)
rp6502_add_test(uni
    SOURCES test_uni.c ${RP6502_SRC}/core/api/uni.c ${OEMCP_C}
        ${RP6502_VENDOR}/fatfs/ffunicode.c
    INCLUDES ${RP6502_SRC}/host/pocket ${RP6502_SRC} ${RP6502_VENDOR}
        ${OEMCP_DIR})
add_dependencies(test_uni oemcp)
set_source_files_properties(${RP6502_VENDOR}/fatfs/ffunicode.c
    TARGET_DIRECTORY test_uni PROPERTIES COMPILE_DEFINITIONS
    "ff_oem2uni=ref_oem2uni;ff_uni2oem=ref_uni2oem;ff_wtoupper=ref_wtoupper")

# --- The keyboard layouts (core/hid/layout.c) against the def files they are
# generated from. def/keyboard_*.def is what a contributor edits and what keyboard.c
# used to include as X macros; the generator is a second reader of that
# format, so the manifest is included here the old way and every code
# point compared. ---
set_source_files_properties(${KBDLAY_C} PROPERTIES GENERATED TRUE)
rp6502_add_test(layout
    SOURCES test_layout.c ${RP6502_SRC}/core/hid/layout.c ${KBDLAY_C}
    INCLUDES ${RP6502_SRC}/host/pocket ${RP6502_SRC} ${KBDLAY_DIR})
add_dependencies(test_layout kbdlay)

# --- The same two suites over the ports that stage a binary ---
# uni_word and layout_word are the one thing a port writes: one links the
# generated C and reads an array, one stages the .bin and reads a window.
# Both come out of one generator, so these are the suites above over the
# other port — only a tree that builds the assets has one to be wrong about.
if(OEMCP_BIN)
    rp6502_add_test(uni_bin
        SOURCES test_uni.c ${RP6502_SRC}/core/api/uni.c uni_bin.c
            ${RP6502_VENDOR}/fatfs/ffunicode.c
        INCLUDES ${RP6502_SRC} ${RP6502_VENDOR} ${OEMCP_DIR}
        DEFS OEMCP_BIN_PATH="${OEMCP_BIN}")
    add_dependencies(test_uni_bin oemcp oemcp_bin)
    set_source_files_properties(${RP6502_VENDOR}/fatfs/ffunicode.c
        TARGET_DIRECTORY test_uni_bin PROPERTIES COMPILE_DEFINITIONS
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
    # The whole machine so far boots a program that talks through the RIA's
    # bare UART.
    rp6502_add_machine_test(hello TIMEOUT 120)
endif()

# Op 0x01 and the attribute API: the console stream those probes print, on
# whichever machine this tree built.
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
    # The .rp6502 loader against the staged window, the testbench playing
    # the platform bridge.
    rp6502_add_machine_test(rom FIRMWARE TIMEOUT 900)
endif()

# The console read raw, which the line editor is not in: the program holds
# TTY: open and echoes what arrives, so a typed byte reaching it is the
# whole claim. Its script is the file that assembles it.
if(TARGET rp6502-emu)
    rp6502_add_script_test(tty DRIVER ${RP6502_TESTS_DIR}/gen/tty_rom_gen.py
        ROM ${TTY_ROM} DEPENDS tty_rom TIMEOUT 120)

    # The ANSI keyboard against the line editor, through the shipped binary:
    # type and wait against a program that reads whole lines. test_adventure
    # above injects on stdin, so the two prove different halves of one path.
    rp6502_add_script_test(adventure FIXTURE adventure.rp6502)
endif()
