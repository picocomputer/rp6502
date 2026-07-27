# Simulation: the verilated machine and the emu_core oracle in one process.
# Uses utest.h (vendored with sokol), like tests/.

set(UTEST_DIR ${RP6502_VENDOR}/sokol/tests/functional)
set(ROMS ${RP6502_ROOT}/tests/roms)

# --- The reference oracle (emu_core driven headless) ---
add_library(fpga_oracle STATIC oracle.c)
target_link_libraries(fpga_oracle PUBLIC emu_core)
target_include_directories(fpga_oracle PUBLIC ${CMAKE_CURRENT_LIST_DIR})

add_executable(test_oracle tests/test_oracle.cpp)
target_link_libraries(test_oracle PRIVATE fpga_oracle)
target_include_directories(test_oracle PRIVATE ${UTEST_DIR})
target_compile_definitions(test_oracle PRIVATE ADVENTURE_ROM="${ROMS}/adventure.rp6502")
add_test(NAME oracle COMMAND test_oracle)

# --- The verilated machine ---
# Platform wrappers under platform/ are not verilated; the simulation models the
# host bridge in C++ instead, so one harness serves every FPGA target.
find_package(verilator CONFIG QUIET HINTS $ENV{VERILATOR_ROOT} /usr/share/verilator)
if(verilator_FOUND)
    add_executable(test_rtl tests/test_rtl.cpp tb_core.cpp)
    target_include_directories(test_rtl PRIVATE ${UTEST_DIR} ${CMAKE_CURRENT_LIST_DIR})
    verilate(test_rtl
        SOURCES
            ${RP6502_FPGA_RTL}/core/rp6502_pkg.sv
            ${RP6502_FPGA_RTL}/core/rp6502.sv
        TOP_MODULE rp6502
        PREFIX Vrp6502
        TRACE_FST
        VERILATOR_ARGS -Wall --assert
    )
    add_test(NAME rtl COMMAND test_rtl)
else()
    message(WARNING
        "Verilator not found — RTL tests skipped, only the oracle builds.\n"
        "  sudo apt-get install verilator")
endif()
