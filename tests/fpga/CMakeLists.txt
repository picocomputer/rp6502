include(${CMAKE_CURRENT_LIST_DIR}/../rp6502_tests.cmake)

# --- The generator agrees with the C it generates from ---
# cpu65's decode tables come from vendor/chips_rp6502, so an upstream change to
# the addressing or the cycle sequences has to be modelled here before it can
# reach the RTL. The generator fails on anything it does not recognise.
set(CPU65_GEN ${RP6502_SRC}/fpga/codegen/cpu65_gen.py)
add_test(NAME cpu65_gen
    COMMAND ${CMAKE_COMMAND} -E env python3 ${CPU65_GEN} --report)

# The generated decode table is built, not committed. verilate() reads its
# sources at configure time, so the table is generated here as well as by the
# build-time rule that keeps it fresh when the generator changes.
set(CPU65_ROM ${CMAKE_CURRENT_BINARY_DIR}/cpu65_rom_pkg.sv)
execute_process(
    COMMAND python3 ${CPU65_GEN} --emit ${CPU65_ROM}
    RESULT_VARIABLE CPU65_GEN_RC)
if(CPU65_GEN_RC)
    message(FATAL_ERROR "cpu65_gen.py failed")
endif()
add_custom_command(OUTPUT ${CPU65_ROM}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${CPU65_GEN} --emit ${CPU65_ROM}
    DEPENDS ${CPU65_GEN}
    COMMENT "Generating the cpu65 decode table"
    VERBATIM)
add_custom_target(cpu65_rom ALL DEPENDS ${CPU65_ROM})

# --- The reference oracle (emu_core driven headless) ---
# Every RTL comparison test runs the same program here and on the verilated
# machine, so emu_core defines the behavior the RTL has to reproduce.
add_library(fpga_oracle STATIC oracle.c)
target_link_libraries(fpga_oracle PUBLIC emu_core)
target_include_directories(fpga_oracle PUBLIC ${CMAKE_CURRENT_LIST_DIR})

rp6502_add_test(oracle
    SOURCES test_oracle.cpp
    LIBS fpga_oracle FIXTURE adventure.rp6502)

# --- The verilated machine ---
# Platform wrappers under src/fpga/platform are not verilated; the simulation
# models the host bridge in C++ instead, so one harness serves every target.
find_package(verilator CONFIG QUIET HINTS $ENV{VERILATOR_ROOT} /usr/share/verilator)
if(verilator_FOUND)
    rp6502_add_test(rtl
        SOURCES test_rtl.cpp tb_core.cpp
        INCLUDES ${CMAKE_CURRENT_LIST_DIR})
    verilate(test_rtl
        SOURCES
            ${RP6502_FPGA_RTL}/core/rp6502_pkg.sv
            ${RP6502_FPGA_RTL}/core/rp6502.sv
        TOP_MODULE rp6502
        PREFIX Vrp6502
        TRACE_FST
        VERILATOR_ARGS -Wall --assert
    )

    # --- The 65C02 in RTL, against the same suites as the emulator's C ---
    # tests/cpu holds both implementations to one set of evidence; this is the
    # RTL half. Fixture guards mirror tests/cpu's.
    set(KLAUS ${RP6502_ROOT}/vendor/6502_65C02_functional_tests/bin_files)
    if(RP6502_CPU_VECTORS AND EXISTS ${KLAUS}/6502_functional_test.bin)
        rp6502_add_test(cpu65
            SOURCES test_cpu65.cpp
                ${RP6502_ROOT}/tests/cpu/vec.c ${RP6502_ROOT}/tests/cpu/klaus.c
            INCLUDES ${RP6502_ROOT}/tests/cpu
            DEFS
                VECTORS="${RP6502_CPU_VECTORS}"
                KLAUS_6502="${KLAUS}/6502_functional_test.bin"
                KLAUS_65C02="${KLAUS}/65C02_extended_opcodes_test.bin"
            TIMEOUT 900)
        add_dependencies(test_cpu65 cpu_vectors cpu65_rom)
        verilate(test_cpu65
            SOURCES ${CPU65_ROM} ${RP6502_FPGA_RTL}/cpu65/cpu65.sv
            TOP_MODULE cpu65
            PREFIX Vcpu65
            VERILATOR_ARGS -Wall --assert -O3
        )
    else()
        message(WARNING "CPU fixtures absent — cpu65 RTL conformance skipped.")
    endif()
else()
    message(WARNING
        "Verilator not found — RTL tests skipped, only the oracle builds.\n"
        "  sudo apt-get install verilator")
endif()
