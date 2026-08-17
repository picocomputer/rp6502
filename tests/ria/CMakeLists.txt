# The RIA: the xreg dispatch, the API surface, the ROM loader and the clock.

# --- The two assemblers are one instruction set ---
# A .rp6502 is built by a generator writing a file and by a bench
# building one in memory, so the encoding has a Python spelling and a
# C++ one. asm_ref.py writes the reference; test_asm.cpp writes the same
# program the other way and compares the images byte for byte. No
# emulator and no simulator, so it runs wherever the suite does.
set(ASM_REF ${CMAKE_CURRENT_BINARY_DIR}/asm_ref.rp6502)
add_custom_command(OUTPUT ${ASM_REF}
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${CMAKE_CURRENT_LIST_DIR}/asm_ref.py --emit ${ASM_REF}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/asm_ref.py
        ${RP6502_ROOT}/src/gen/rp6502_rom.py
    COMMENT "Assembling the reference image"
    VERBATIM)
add_custom_target(asm_ref DEPENDS ${ASM_REF})
rp6502_add_test(asm SOURCES test_asm.cpp DEFS ASM_REF="${ASM_REF}")
add_dependencies(test_asm asm_ref)

# --- Unit tests (crc32, .rp6502 loader, xreg dispatch, CLI parser) ---
rp6502_add_test(units
    SOURCES test_units.c ${RP6502_SRC}/emu/app/cli.c
    LIBS emu_core FIXTURE adventure.rp6502)
if(WIN32) # cli.c needs the same getopt the app uses
    target_link_libraries(test_units PRIVATE wingetopt)
    target_compile_definitions(test_units PRIVATE _BSD_SOURCE)
endif()

# --- Feature interfaces ($FFF0 SIGINT IRQ, launcher chain, teletype bell) ---
rp6502_add_test(features LIBS emu_core FIXTURE adventure.rp6502)

# --- Adventure integration (ROM-asset files + interactive stdin via rln) ---
rp6502_add_test(adventure LIBS emu_core FIXTURE adventure.rp6502 TIMEOUT 60)

# --- Exec/argv (pro.c ops 0x08/0x09) + MSC0: program loading ---
rp6502_add_test(exec LIBS emu_core FIXTURE exec.rp6502 TIMEOUT 60)

# --- Clock/time API (host wall clock + timezone + locale + OEM code page) ---
rp6502_add_test(rtc LIBS emu_core FIXTURE rtc.rp6502 TIMEOUT 60)

# --- Master clock + PHI2 in the emulator (reproducible run timing) ---
rp6502_add_test(clock LIBS emu_core FIXTURE adventure.rp6502)

# --- OEM code page conversion (ria/api/uni.c) against the ffunicode.c it
# replaces. The vendored file is compiled beside it with its entry points
# renamed, so every code page, every byte, every code point in the BMP,
# and the whole up-case map are compared in one process. ---
set_source_files_properties(${OEMCP_C} PROPERTIES GENERATED TRUE)
rp6502_add_test(uni
    SOURCES test_uni.c ${RP6502_SRC}/ria/api/uni.c ${OEMCP_C}
        ${RP6502_VENDOR}/fatfs/ffunicode.c
    INCLUDES ${RP6502_SRC} ${RP6502_VENDOR} ${RP6502_SRC}/host/pico
        ${OEMCP_DIR})
add_dependencies(test_uni oemcp)
set_source_files_properties(${RP6502_VENDOR}/fatfs/ffunicode.c
    TARGET_DIRECTORY test_uni PROPERTIES COMPILE_DEFINITIONS
    "ff_oem2uni=ref_oem2uni;ff_uni2oem=ref_uni2oem;ff_wtoupper=ref_wtoupper")

# --- The same suite over the Pocket's copy of the tables ---
# uni_word is the one thing a port writes, and there are two ports: one
# links oemcp.c and reads an array, one stages oemcp.bin and reads a
# window. Both come out of one generator and nothing compared them, so
# uni_bin.c is the second port and this is test_uni.c over it. Only the
# tree that builds the .bin can run it; the emulator has no staged
# tables to be wrong about.
if(OEMCP_BIN)
    rp6502_add_test(uni_bin
        SOURCES test_uni.c ${RP6502_SRC}/ria/api/uni.c uni_bin.c
            ${RP6502_VENDOR}/fatfs/ffunicode.c
        INCLUDES ${RP6502_SRC} ${RP6502_VENDOR} ${RP6502_SRC}/host/pico
            ${OEMCP_DIR}
        DEFS OEMCP_BIN_PATH="${OEMCP_BIN}")
    add_dependencies(test_uni_bin oemcp oemcp_bin)
    set_source_files_properties(${RP6502_VENDOR}/fatfs/ffunicode.c
        TARGET_DIRECTORY test_uni_bin PROPERTIES COMPILE_DEFINITIONS
        "ff_oem2uni=ref_oem2uni;ff_uni2oem=ref_uni2oem;ff_wtoupper=ref_wtoupper")
endif()

# --- The keyboard layouts (ria/hid/kbl.c) against the def files they are
# generated from. def/kbd_*.def is what a contributor edits and what kbd.c
# used to include as X macros; the generator is a second reader of that
# format, so the manifest is included here the old way and every code
# point compared. ---
set_source_files_properties(${KBDLAY_C} PROPERTIES GENERATED TRUE)
rp6502_add_test(kbl
    SOURCES test_kbl.c ${RP6502_SRC}/ria/hid/kbl.c ${KBDLAY_C}
    INCLUDES ${RP6502_SRC} ${RP6502_SRC}/host/pico ${KBDLAY_DIR})
add_dependencies(test_kbl kbdlay)

# --- The same suite over the Pocket's copy of the layouts ---
# kbl_word is the one thing a port writes: one links kbdlay.c and reads
# an array, one stages keyboard.bin and reads a window. Only the tree that
# builds the .bin can run this.
if(KBDLAY_BIN)
    rp6502_add_test(kbl_bin
        SOURCES test_kbl.c ${RP6502_SRC}/ria/hid/kbl.c kbl_bin.c
        INCLUDES ${RP6502_SRC} ${RP6502_SRC}/host/pico ${KBDLAY_DIR}
        DEFS KBDLAY_BIN_PATH="${KBDLAY_BIN}")
    add_dependencies(test_kbl_bin kbdlay kbdlay_bin)
endif()

if(RP6502_VERILATE)
    # --- Every whole kilohertz, exactly ---
    rp6502_add_module_test(phi2
        TOP phi2_div
        RTL ${RP6502_SRC}/rtl/ria/phi2_div.sv
        TIMEOUT 120)

    # The whole machine so far boots a program that talks through the RIA's
    # bare UART.
    rp6502_add_machine_test(hello TIMEOUT 120)
endif()

if(RP6502_SOFT_CPU)
    # Op 0x01 dispatch against the oracle, and the RTL program it lands.
    rp6502_add_machine_test(xreg ORACLE FIRMWARE TIMEOUT 900)

    # The .rp6502 loader against the staged window, the testbench playing
    # the platform bridge.
    rp6502_add_machine_test(rom FIRMWARE TIMEOUT 900)
endif()
