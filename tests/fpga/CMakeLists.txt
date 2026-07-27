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

# The font ROM comes from vga/term/font.c the same way: generated at
# configure time for verilate(), kept fresh by the build-time rule, and
# checked byte for byte against font_init's runtime tables by test_font.
set(VID_FONT_GEN ${RP6502_SRC}/fpga/codegen/vid_font_gen.py)
set(VID_FONT_PKG ${CMAKE_CURRENT_BINARY_DIR}/vid_font_pkg.sv)
set(VID_FONT_H ${CMAKE_CURRENT_BINARY_DIR}/vid_font_tables.h)
execute_process(
    COMMAND python3 ${VID_FONT_GEN} --emit-sv ${VID_FONT_PKG} --emit-h ${VID_FONT_H}
    RESULT_VARIABLE VID_FONT_GEN_RC)
if(VID_FONT_GEN_RC)
    message(FATAL_ERROR "vid_font_gen.py failed")
endif()
add_custom_command(OUTPUT ${VID_FONT_PKG} ${VID_FONT_H}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${VID_FONT_GEN}
        --emit-sv ${VID_FONT_PKG} --emit-h ${VID_FONT_H}
    DEPENDS ${VID_FONT_GEN} ${RP6502_SRC}/vga/term/font.c
    COMMENT "Generating the vid_font ROM"
    VERBATIM)
add_custom_target(vid_font_rom ALL DEPENDS ${VID_FONT_PKG} ${VID_FONT_H})

# --- The reference oracle (emu_core driven headless) ---
# Every RTL comparison test runs the same program here and on the verilated
# machine, so emu_core defines the behavior the RTL has to reproduce.
add_library(fpga_oracle STATIC oracle.c)
target_link_libraries(fpga_oracle PUBLIC emu_core)
target_include_directories(fpga_oracle PUBLIC ${CMAKE_CURRENT_LIST_DIR})

rp6502_add_test(oracle
    SOURCES test_oracle.cpp
    LIBS fpga_oracle FIXTURE adventure.rp6502)

rp6502_add_test(font
    SOURCES test_font.cpp
    INCLUDES ${CMAKE_CURRENT_BINARY_DIR}
    LIBS fpga_oracle)
add_dependencies(test_font vid_font_rom)

# --- The verilated machine ---
# Platform wrappers under src/fpga/platform are not verilated; the simulation
# models the host bridge in C++ instead, so one harness serves every target.
find_package(verilator CONFIG QUIET HINTS $ENV{VERILATOR_ROOT} /usr/share/verilator)
if(verilator_FOUND)
    set(RP6502_MACHINE_SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/hazard3.vlt
        ${CPU65_ROM}
        ${RP6502_FPGA_RTL}/core/rp6502_pkg.sv
        ${RP6502_FPGA_RTL}/cpu65/cpu65.sv
        ${RP6502_FPGA_RTL}/cpu65/via.sv
        ${RP6502_FPGA_RTL}/cpu65/phi2_div.sv
        ${RP6502_FPGA_RTL}/mem/sram64k.sv
        ${RP6502_FPGA_RTL}/ria/ria_regs.sv
        ${RP6502_FPGA_RTL}/rv/rv_soc.sv
        ${RP6502_FPGA_RTL}/vid/vid_timing.sv
        ${RP6502_FPGA_RTL}/vid/vid_term.sv
        ${RP6502_FPGA_RTL}/core/rp6502.sv)
    set(RP6502_MACHINE_VERILATOR_ARGS
        -y ${RP6502_VENDOR}/hazard3/hdl
        -y ${RP6502_VENDOR}/hazard3/hdl/arith
        -y ${RP6502_VENDOR}/hazard3/hdl/debug/dm
        -y ${RP6502_VENDOR}/hazard3/hdl/debug/dtm)

    rp6502_add_test(rtl
        SOURCES test_rtl.cpp tb_core.cpp
        INCLUDES ${CMAKE_CURRENT_LIST_DIR})
    add_dependencies(test_rtl cpu65_rom)
    verilate(test_rtl
        SOURCES ${RP6502_MACHINE_SOURCES}
        TOP_MODULE rp6502
        PREFIX Vrp6502
        TRACE_FST
        INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
        VERILATOR_ARGS -Wall --assert ${RP6502_MACHINE_VERILATOR_ARGS}
    )

    # The whole machine so far boots a program that talks through the UART.
    rp6502_add_test(hello
        SOURCES test_hello.cpp
        INCLUDES ${CMAKE_CURRENT_LIST_DIR}
        TIMEOUT 120)
    add_dependencies(test_hello cpu65_rom)
    verilate(test_hello
        SOURCES ${RP6502_MACHINE_SOURCES}
        TOP_MODULE rp6502
        PREFIX Vrp6502
        INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
        VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
    )

    # The frame counter and the vsync interrupt against the raster.
    rp6502_add_test(vsync
        SOURCES test_vsync.cpp
        INCLUDES ${CMAKE_CURRENT_LIST_DIR}
        TIMEOUT 120)
    add_dependencies(test_vsync cpu65_rom)
    verilate(test_vsync
        SOURCES ${RP6502_MACHINE_SOURCES}
        TOP_MODULE rp6502
        PREFIX Vrp6502
        INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
        VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
    )

    # --- The 65C02 in RTL, against the same suites as the emulator's C ---
    # tests/cpu holds both implementations to one set of evidence; this is the
    # RTL half. Fixture guards mirror tests/cpu's.
    set(KLAUS ${RP6502_ROOT}/vendor/6502_65C02_functional_tests/bin_files)
    if(RP6502_CPU_VECTORS AND EXISTS ${KLAUS}/6502_functional_test.bin)
        rp6502_add_test(cpu65
            SOURCES test_cpu65.cpp cpu65_dut.cpp
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

        # Interrupts, reset, RDY, WAI and STP: the C emulation and the RTL in
        # lockstep under scripted pins — coverage the vectors cannot express.
        rp6502_add_test(lockstep
            SOURCES test_lockstep.cpp cpu65_dut.cpp
                ${RP6502_ROOT}/tests/cpu/chips_dut.c
                ${RP6502_ROOT}/tests/cpu/lockstep.c
            INCLUDES ${RP6502_ROOT}/tests/cpu
                ${RP6502_VENDOR}/chips_rp6502 ${RP6502_VENDOR}
            TIMEOUT 300)
        add_dependencies(test_lockstep cpu65_rom)
        verilate(test_lockstep
            SOURCES ${CPU65_ROM} ${RP6502_FPGA_RTL}/cpu65/cpu65.sv
            TOP_MODULE cpu65
            PREFIX Vcpu65
            VERILATOR_ARGS -Wall --assert -O3
        )
    else()
        message(WARNING "CPU fixtures absent — cpu65 RTL conformance skipped.")
    endif()

    # --- The soft CPU: Hazard3 boots the cross-compiled firmware ---
    # Toolchain per README: apt install gcc-riscv64-unknown-elf.
    find_program(RISCV_GCC riscv64-unknown-elf-gcc)
    find_program(RISCV_OBJCOPY riscv64-unknown-elf-objcopy)
    if(RISCV_GCC AND RISCV_OBJCOPY)
        set(SW_SRC ${RP6502_SRC}/fpga/sw)
        set(SW_BIN ${CMAKE_CURRENT_BINARY_DIR}/sw.bin)
        set(SW_SOURCES
            ${SW_SRC}/crt0.S ${SW_SRC}/main.c ${SW_SRC}/com.c
            ${SW_SRC}/font.c ${SW_SRC}/mem.c ${SW_SRC}/rom.c ${SW_SRC}/time.c
            ${SW_SRC}/trap.c ${SW_SRC}/vga.c ${SW_SRC}/vid.c
            ${RP6502_SRC}/ria/api/api.c
            ${RP6502_SRC}/ria/api/std.c
            ${RP6502_SRC}/ria/str/rln.c
            ${RP6502_SRC}/ria/str/str.c
            ${RP6502_SRC}/vga/term/color.c
            ${RP6502_SRC}/vga/term/term.c)
        add_custom_command(OUTPUT ${SW_BIN}
            COMMAND ${RISCV_GCC} -march=rv32imac_zicsr_zifencei -mabi=ilp32
                -Os -ffreestanding -nostartfiles
                --specs=picolibc.specs -DPICOLIBC_INTEGER_PRINTF_SCANF
                -ffunction-sections -fdata-sections -Wl,--gc-sections
                -I ${RP6502_SRC}
                -I ${SW_SRC}/shim
                -I ${RP6502_SRC}/emu/host/pico
                -I ${RP6502_VENDOR}
                "-DPICO_PROGRAM_NAME=\"RP6502-FPGA\""
                -T ${SW_SRC}/link.ld -Wl,--no-warn-rwx-segments
                -o ${CMAKE_CURRENT_BINARY_DIR}/sw.elf
                ${SW_SOURCES}
            COMMAND ${RISCV_OBJCOPY} -O binary
                ${CMAKE_CURRENT_BINARY_DIR}/sw.elf ${SW_BIN}
            DEPENDS ${SW_SOURCES} ${SW_SRC}/link.ld
            COMMENT "Cross-compiling the soft CPU firmware"
            VERBATIM)
        add_custom_target(sw_bin DEPENDS ${SW_BIN})

        # The real boot path: no backdoor into the 6502's world, only the
        # firmware image, the way a bitstream carries it.
        rp6502_add_test(boot
            SOURCES test_boot.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            DEFS SW_BIN="${SW_BIN}"
            TIMEOUT 120)
        add_dependencies(test_boot sw_bin cpu65_rom)
        verilate(test_boot
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # The M2 exit test: the same .rp6502 on the RTL machine and on
        # emu_core, console streams compared byte for byte.
        rp6502_add_test(compare
            SOURCES test_compare.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}"
            TIMEOUT 120)
        add_dependencies(test_compare sw_bin cpu65_rom)
        verilate(test_compare
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # The .rp6502 loader against the staged window, the testbench
        # playing the platform bridge.
        rp6502_add_test(rom
            SOURCES test_rom.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            DEFS SW_BIN="${SW_BIN}"
            TIMEOUT 120)
        add_dependencies(test_rom sw_bin cpu65_rom)
        verilate(test_rom
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )
    else()
        message(WARNING
            "riscv64-unknown-elf-gcc not found — soft CPU tests skipped.\n"
            "  sudo apt-get install gcc-riscv64-unknown-elf picolibc-riscv64-unknown-elf")
    endif()

    # --- The VIA in RTL, against chips/m6522.h in lockstep ---
    # T1/T2 timers and their interrupts are live machine features; see
    # emu/emu/via.c for the wiring this reproduces.
    rp6502_add_test(via
        SOURCES test_via.cpp
        INCLUDES ${RP6502_VENDOR}/chips_rp6502 ${RP6502_VENDOR}
        TIMEOUT 300)
    verilate(test_via
        SOURCES ${RP6502_FPGA_RTL}/cpu65/via.sv
        TOP_MODULE via
        PREFIX Vvia
        VERILATOR_ARGS -Wall --assert -O3
    )
else()
    message(WARNING
        "Verilator not found — RTL tests skipped, only the oracle builds.\n"
        "  sudo apt-get install verilator")
endif()
