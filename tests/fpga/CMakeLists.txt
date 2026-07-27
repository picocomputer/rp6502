include(${CMAKE_CURRENT_LIST_DIR}/../rp6502_tests.cmake)

# --- The generator agrees with the C it generates from ---
# cpu65's decode tables come from vendor/chips_rp6502, so an upstream change to
# the addressing or the cycle sequences has to be modelled here before it can
# reach the RTL. The generator fails on anything it does not recognise.
set(CPU65_GEN ${RP6502_SRC}/fpga/codegen/cpu65_gen.py)
add_test(NAME cpu65_gen
    COMMAND ${CMAKE_COMMAND} -E env python3 ${CPU65_GEN} --report)

# The generated decode table is built, not committed, and has to be valid RTL.
set(CPU65_ROM ${CMAKE_CURRENT_BINARY_DIR}/cpu65_rom_pkg.sv)
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

    # Nothing instantiates the decode table yet, so lint it on its own until
    # cpu65 does.
    find_program(VERILATOR_EXE verilator REQUIRED)
    add_test(NAME cpu65_rom_lint
        COMMAND ${VERILATOR_EXE} --lint-only -Wall ${CPU65_ROM})
else()
    message(WARNING
        "Verilator not found — RTL tests skipped, only the oracle builds.\n"
        "  sudo apt-get install verilator")
endif()
