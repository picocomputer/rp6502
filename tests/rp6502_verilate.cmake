# The RTL half of the suite: the simulator, and two helpers that hide it.
#
# Only what genuinely needs Verilator is here. What to verilate — the machine's
# source list — and the soft CPU firmware are in src/rtl/machine.cmake, because a
# Quartus project needs both and needs no simulator to read them.
#
# Included only when RP6502_RTL_SIM is on. A machine without Verilator or
# without the RISC-V toolchain still configures and still runs every C test; it
# just registers fewer of them.
#
# RP6502_VERILATE  verilator is present; RTL tests can be registered.

# --- The reference oracle (emu_core driven headless) ---
# Every RTL comparison test runs the same program here and on the verilated
# machine, so emu_core defines the behavior the RTL has to reproduce.
add_library(fpga_oracle STATIC ${RP6502_BENCH}/oracle.c)
target_link_libraries(fpga_oracle PUBLIC emu_core)
target_include_directories(fpga_oracle PUBLIC ${RP6502_BENCH})

# --- The verilated machine ---
# Host wrappers under src/host are not verilated; the simulation
# models the host bridge in C++ instead, so one harness serves every target.
find_package(verilator CONFIG QUIET HINTS $ENV{VERILATOR_ROOT} /usr/share/verilator)
if(NOT verilator_FOUND)
    # No early return: this file is include()d, so returning here would skip
    # the rest of the caller as well.
    message(WARNING
        "Verilator not found — RTL tests skipped, only the C suite builds.\n"
        "  sudo apt-get install verilator")
endif()
if(verilator_FOUND)
set(RP6502_VERILATE ON)

# The waivers ride with the simulator rather than with the manifest in
# rtl.cmake, because that is what they are: Verilator's own lint, waived.
# Quartus never sees them — its lists are filtered by extension — and a
# tree built without a simulator has no business naming a file in tests/.
#
# They lead, because a waiver read after the file it waives is not applied.
list(PREPEND OPL2_SOURCES ${RP6502_BENCH}/opl2.vlt)
list(PREPEND RP6502_MACHINE_SOURCES
    ${RP6502_BENCH}/hazard3.vlt ${RP6502_BENCH}/opl2.vlt)
endif() # verilator_FOUND

# rp6502_add_module_test(<name> TOP <module> [PREFIX <V...>] RTL <file>...
#                        [SOURCES ...] [INCLUDES ...] [DEFS ...] [LIBS ...]
#                        [DEPENDS ...] [ARGS <verilator arg>...] [TIMEOUT n])
#
# One RTL module verilated on its own, against C that drives it directly.
function(rp6502_add_module_test name)
    cmake_parse_arguments(U "" "TOP;PREFIX;TIMEOUT"
        "RTL;SOURCES;INCLUDES;DEFS;LIBS;DEPENDS;ARGS" ${ARGN})
    if(NOT U_SOURCES)
        set(U_SOURCES test_${name}.cpp)
    endif()
    if(NOT U_PREFIX)
        set(U_PREFIX V${U_TOP})
    endif()
    # Built up rather than passed through: a bare TIMEOUT with nothing after
    # it is a parse warning, not a no-op.
    set(_args SOURCES ${U_SOURCES})
    foreach(kw INCLUDES DEFS LIBS TIMEOUT)
        if(U_${kw})
            list(APPEND _args ${kw} ${U_${kw}})
        endif()
    endforeach()
    rp6502_add_test(${name} ${_args})
    if(U_DEPENDS)
        add_dependencies(test_${name} ${U_DEPENDS})
    endif()
    verilate(test_${name}
        SOURCES ${U_RTL}
        TOP_MODULE ${U_TOP}
        PREFIX ${U_PREFIX}
        VERILATOR_ARGS -Wall --assert -O3 ${U_ARGS})
endfunction()

# rp6502_add_machine_test(<name> [ORACLE] [TRACE] [FIRMWARE] [SOURCES ...]
#                         [DEFS ...] [RTL <extra>...] [TOP <m>] [PREFIX <V>]
#                         [DEPENDS ...] [TIMEOUT n])
#
# The whole machine. FIRMWARE means the test boots the soft CPU rather than
# staging the 6502's memory directly, so it needs the cross-compiled image and
# the assets that image loads. Those definitions and the targets that produce
# them come with the keyword rather than being restated at sixteen call sites,
# which is how one of them came to name FONTS_BIN without depending on the
# rule that writes it.
function(rp6502_add_machine_test name)
    cmake_parse_arguments(M "ORACLE;TRACE;FIRMWARE" "TOP;PREFIX;TIMEOUT"
        "SOURCES;DEFS;RTL;DEPENDS" ${ARGN})
    if(NOT M_SOURCES)
        set(M_SOURCES test_${name}.cpp)
    endif()
    if(NOT M_TOP)
        set(M_TOP rp6502)
    endif()
    if(NOT M_PREFIX)
        set(M_PREFIX V${M_TOP})
    endif()

    set(_args SOURCES ${M_SOURCES} INCLUDES ${RP6502_BENCH} ${RP6502_ASSETS} ${RP6502_SRC})
    set(_deps cpu65_rom ${M_DEPENDS})
    if(M_ORACLE)
        list(APPEND _args LIBS fpga_oracle)
    endif()
    if(M_FIRMWARE)
        list(APPEND _args DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
            OEMCP_BIN="${OEMCP_BIN}" ${M_DEFS})
        list(APPEND _deps sw_bin vid_font_rom vid_palette_rom oemcp_bin)
    elseif(M_DEFS)
        list(APPEND _args DEFS ${M_DEFS})
    endif()
    if(M_TIMEOUT)
        list(APPEND _args TIMEOUT ${M_TIMEOUT})
    endif()
    rp6502_add_test(${name} ${_args})
    add_dependencies(test_${name} ${_deps})

    # TRACE_FST costs simulation speed, so only the test that reads waveforms
    # asks for it; the rest take -O3 instead.
    set(_trace)
    set(_opt -O3)
    if(M_TRACE)
        set(_trace TRACE_FST)
        set(_opt)
    endif()
    verilate(test_${name}
        SOURCES ${RP6502_MACHINE_SOURCES} ${M_RTL}
        TOP_MODULE ${M_TOP}
        PREFIX ${M_PREFIX}
        ${_trace}
        INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
        VERILATOR_ARGS -Wall --assert ${_opt} ${RP6502_MACHINE_VERILATOR_ARGS})
endfunction()
