# The RIA: the xreg dispatch, the API surface, the ROM loader and the clock.

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

if(RP6502_VERILATE)
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
