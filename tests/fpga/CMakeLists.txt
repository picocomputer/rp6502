include(${CMAKE_CURRENT_LIST_DIR}/../rp6502_tests.cmake)

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
else()
    message(WARNING
        "Verilator not found — RTL tests skipped, only the oracle builds.\n"
        "  sudo apt-get install verilator")
endif()
