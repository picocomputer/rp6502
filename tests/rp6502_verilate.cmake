# The RTL half of the suite: what to verilate, and two helpers that hide it.
#
# Included only by src/fpga, which is the tree that has RTL. Everything below
# is guarded, so a machine without verilator or the RISC-V toolchain still
# configures and still runs every C test — it just registers fewer of them.
#
# RP6502_VERILATE  verilator is present; RTL tests can be registered.
# RP6502_SOFT_CPU  the RISC-V toolchain is present too, so tests that need a
#                  booted soft CPU can be registered.

include(${CMAKE_CURRENT_LIST_DIR}/rp6502_assets.cmake)

# --- The reference oracle (emu_core driven headless) ---
# Every RTL comparison test runs the same program here and on the verilated
# machine, so emu_core defines the behavior the RTL has to reproduce.
add_library(fpga_oracle STATIC ${RP6502_BENCH}/oracle.c)
target_link_libraries(fpga_oracle PUBLIC emu_core)
target_include_directories(fpga_oracle PUBLIC ${RP6502_BENCH})

# --- The verilated machine ---
# Platform wrappers under src/fpga/platform are not verilated; the simulation
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

# The OPL2 is vendored under LGPL-3.0 and credited in the Pocket
# distribution README. Our fixes shadow their originals by being named
# first and the vendor copies dropped from the glob, so the submodule
# stays untouched. The package has to lead; nothing else cares.
#
# Every file is listed rather than found on a search path, because
# Quartus resolves .name port shorthand only against modules it has
# already been given, and the OPL2's memory wrappers are written that
# way. i2s is the dev board's audio out, and we have our own.
set(OPL2_DIR ${RP6502_VENDOR}/opl2_fpga/fpga/modules)
set(OPL2_SOURCES
    ${RP6502_BENCH}/opl2.vlt
    ${RP6502_VENDOR}/opl2_fpga_rp6502/opl2_pkg.sv
    ${RP6502_VENDOR}/opl2_fpga_rp6502/mem_single_bank.sv
    ${RP6502_VENDOR}/opl2_fpga_rp6502/mem_simple_dual_port.sv
    ${RP6502_VENDOR}/opl2_fpga_rp6502/trick_sw_detection.sv
    ${RP6502_VENDOR}/opl2_fpga_rp6502/afifo.v)
foreach(dir top_level channels operator timers host_if misc clks)
    file(GLOB _opl_dir_src ${OPL2_DIR}/${dir}/src/*.sv ${OPL2_DIR}/${dir}/src/*.v)
    list(FILTER _opl_dir_src EXCLUDE REGEX
        "/(i2s|mem_single_bank|mem_simple_dual_port|trick_sw_detection)\\.sv$|/afifo\\.v$")
    list(APPEND OPL2_SOURCES ${_opl_dir_src})
endforeach()

set(RP6502_MACHINE_SOURCES
    ${RP6502_BENCH}/hazard3.vlt
    ${RP6502_VENDOR}/hazard3_rp6502/hazard3_regfile_1w2r.v
    ${OPL2_SOURCES}
    ${CPU65_ROM}
    ${RP6502_FPGA_RTL}/core/rp6502_pkg.sv
    ${RP6502_FPGA_RTL}/cpu65/cpu65.sv
    ${RP6502_FPGA_RTL}/cpu65/via.sv
    ${RP6502_FPGA_RTL}/cpu65/phi2_div.sv
    ${RP6502_FPGA_RTL}/mem/sram64k.sv
    ${RP6502_FPGA_RTL}/mem/xram64k.sv
    ${RP6502_FPGA_RTL}/ria/ria_regs.sv
    ${RP6502_FPGA_RTL}/rv/rv_soc.sv
    ${RP6502_FPGA_RTL}/vid/vid_timing.sv
    ${AUD_SINE_PKG}
    ${RP6502_FPGA_RTL}/aud/aud_psg.sv
    ${RP6502_FPGA_RTL}/aud/aud_bel.sv
    ${RP6502_FPGA_RTL}/aud/aud_opl.sv
    ${RSMP_COEF_PKG}
    ${RP6502_FPGA_RTL}/aud/aud_rsmp.sv
    ${VID_PALETTE_PKG}
    ${RP6502_FPGA_RTL}/vid/vid_font.sv
    ${RP6502_FPGA_RTL}/vid/vid_palram.sv
    ${RP6502_FPGA_RTL}/vid/vid_mode.sv
    ${RP6502_FPGA_RTL}/vid/vid_mode1.sv
    ${RP6502_FPGA_RTL}/vid/vid_mode2.sv
    ${RP6502_FPGA_RTL}/vid/vid_mode3.sv
    ${RP6502_FPGA_RTL}/vid/vid_mode4.sv
    ${RP6502_FPGA_RTL}/vid/vid_mode5.sv
    ${RP6502_FPGA_RTL}/vid/vid_palcache.sv
    ${RP6502_FPGA_RTL}/vid/vid_sprite.sv
    ${RP6502_FPGA_RTL}/vid/vid_prog.sv
    ${RP6502_FPGA_RTL}/vid/vid_term.sv
    ${RP6502_FPGA_RTL}/vid/vid_compose.sv
    ${RP6502_FPGA_RTL}/core/rp6502.sv)
set(RP6502_MACHINE_VERILATOR_ARGS
    -y ${RP6502_VENDOR}/hazard3/hdl
    -y ${RP6502_VENDOR}/hazard3/hdl/arith
    -y ${RP6502_VENDOR}/hazard3/hdl/debug/dm
    -y ${RP6502_VENDOR}/hazard3/hdl/debug/dtm)

# --- The soft CPU: Hazard3 boots the cross-compiled firmware ---
# Toolchain per README: apt install gcc-riscv64-unknown-elf.
find_program(RISCV_GCC riscv64-unknown-elf-gcc)
find_program(RISCV_OBJCOPY riscv64-unknown-elf-objcopy)
if(RISCV_GCC AND RISCV_OBJCOPY)
    set(RP6502_SOFT_CPU ON)
    set(SW_SRC ${RP6502_SRC}/fpga/sw)
    set(SW_BIN ${RP6502_ASSETS}/sw.bin)
    # The firmware's own headers carry the hardware's addresses, so a
    # window that moves has to rebuild the image that writes to it.
    file(GLOB SW_HEADERS ${RP6502_SRC}/fpga/sw/*.h)
    set(SW_SOURCES
        ${SW_SRC}/crt0.S ${SW_SRC}/main.c ${SW_SRC}/aud.c ${SW_SRC}/cfg.c
        ${SW_SRC}/com.c ${SW_SRC}/cpu.c ${SW_SRC}/font.c ${SW_SRC}/kbd.c ${SW_SRC}/mem.c
        ${SW_SRC}/mou.c ${SW_SRC}/msc.c ${SW_SRC}/pad.c ${SW_SRC}/pix.c
        ${SW_SRC}/rand.c ${SW_SRC}/rom.c ${SW_SRC}/time.c
        ${SW_SRC}/trap.c ${SW_SRC}/uni.c ${SW_SRC}/vga.c ${SW_SRC}/vid.c
        ${RP6502_SRC}/ria/api/api.c
        ${RP6502_SRC}/ria/api/std.c
        ${RP6502_SRC}/ria/api/uni.c
        ${RP6502_SRC}/ria/str/rln.c
        ${RP6502_SRC}/ria/str/str.c
        ${RP6502_SRC}/vga/modes/mode1.c
        ${RP6502_SRC}/vga/modes/mode2.c
        ${RP6502_SRC}/vga/modes/mode3.c
        ${RP6502_SRC}/vga/modes/mode4.c
        ${RP6502_SRC}/vga/modes/mode5.c
        ${RP6502_SRC}/vga/term/color.c
        ${RP6502_SRC}/vga/term/term.c)
    add_custom_command(OUTPUT ${SW_BIN}
        COMMAND ${RISCV_GCC} -march=rv32imac_zicsr_zifencei -mabi=ilp32
            -Os -ffreestanding -nostartfiles
            --specs=picolibc.specs -DPICOLIBC_INTEGER_PRINTF_SCANF
            -ffunction-sections -fdata-sections -Wl,--gc-sections -flto
            -I ${RP6502_SRC}
            -I ${SW_SRC}/shim
            -I ${RP6502_ASSETS}
            -I ${RP6502_SRC}/emu/host/pico
            -I ${RP6502_VENDOR}
            "-DPICO_PROGRAM_NAME=\"RP6502-FPGA\""
            # The alternate screen buffer doubles term.c's cell store,
            # and on this platform the cells are block memory. Off here
            # buys twenty-eight blocks, which is where the 64 KB TCM
            # comes from. Other fabrics with room leave it on.
            -DTERM_ALT_SCREEN=0
            -T ${SW_SRC}/link.ld -Wl,--no-warn-rwx-segments
            -o ${RP6502_ASSETS}/sw.elf
            ${SW_SOURCES}
        COMMAND ${RISCV_OBJCOPY} -O binary ${RP6502_ASSETS}/sw.elf ${SW_BIN}
        DEPENDS ${SW_SOURCES} ${SW_HEADERS} ${VID_FONT_ASSET_H}
            ${SW_SRC}/link.ld
        COMMENT "Cross-compiling the soft CPU firmware"
        VERBATIM)
    add_custom_target(sw_bin DEPENDS ${SW_BIN})
else()
    message(WARNING
        "riscv64-unknown-elf-gcc not found — soft CPU tests skipped.\n"
        "  sudo apt-get install gcc-riscv64-unknown-elf picolibc-riscv64-unknown-elf")
endif()
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
