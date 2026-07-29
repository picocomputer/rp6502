include(${CMAKE_CURRENT_LIST_DIR}/../rp6502_tests.cmake)

# --- The generator agrees with the C it generates from ---
# cpu65's decode tables come from vendor/chips_rp6502, so an upstream change to
# the addressing or the cycle sequences has to be modelled here before it can
# reach the RTL. The generator fails on anything it does not recognise.
set(CPU65_GEN ${RP6502_SRC}/fpga/codegen/cpu65_gen.py)
add_test(NAME cpu65_gen
    COMMAND ${CMAKE_COMMAND} -E env python3 ${CPU65_GEN} --report)

# The bitstream byte-reversal for the Pocket's rbf_r, an involution.
add_test(NAME rbf_r
    COMMAND ${CMAKE_COMMAND} -E env python3
        ${RP6502_SRC}/fpga/codegen/rbf_r_gen.py --check)

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

# The font asset comes from vga/term/font.c: the image the firmware
# copies into the store, an offsets header for it, and the tables
# test_font checks byte for byte against font_init's runtime image.
# Generated at configure time so the sim can compile against it, kept
# fresh by the build-time rule.
set(VID_FONT_GEN ${RP6502_SRC}/fpga/codegen/vid_font_gen.py)
set(VID_FONT_BIN ${CMAKE_CURRENT_BINARY_DIR}/fonts.bin)
set(VID_FONT_H ${CMAKE_CURRENT_BINARY_DIR}/vid_font_tables.h)
set(VID_FONT_ASSET_H ${CMAKE_CURRENT_BINARY_DIR}/vid_font_asset.h)
execute_process(
    COMMAND python3 ${VID_FONT_GEN} --emit-bin ${VID_FONT_BIN}
        --emit-h ${VID_FONT_H} --emit-asset-h ${VID_FONT_ASSET_H}
    RESULT_VARIABLE VID_FONT_GEN_RC)
if(VID_FONT_GEN_RC)
    message(FATAL_ERROR "vid_font_gen.py failed")
endif()
add_custom_command(OUTPUT ${VID_FONT_BIN} ${VID_FONT_H} ${VID_FONT_ASSET_H}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${VID_FONT_GEN}
        --emit-bin ${VID_FONT_BIN} --emit-h ${VID_FONT_H}
        --emit-asset-h ${VID_FONT_ASSET_H}
    DEPENDS ${VID_FONT_GEN} ${RP6502_SRC}/vga/term/font.c
    COMMENT "Generating the font asset"
    VERBATIM)
add_custom_target(vid_font_rom ALL DEPENDS ${VID_FONT_BIN} ${VID_FONT_H}
    ${VID_FONT_ASSET_H})

# The builtin palettes ride the same way, from vga/term/color.c.
set(VID_PALETTE_GEN ${RP6502_SRC}/fpga/codegen/vid_palette_gen.py)
set(VID_PALETTE_PKG ${CMAKE_CURRENT_BINARY_DIR}/vid_palette_pkg.sv)
set(VID_PALETTE_H ${CMAKE_CURRENT_BINARY_DIR}/vid_palette_tables.h)
execute_process(
    COMMAND python3 ${VID_PALETTE_GEN}
        --emit-sv ${VID_PALETTE_PKG} --emit-h ${VID_PALETTE_H}
    RESULT_VARIABLE VID_PALETTE_GEN_RC)
if(VID_PALETTE_GEN_RC)
    message(FATAL_ERROR "vid_palette_gen.py failed")
endif()
add_custom_command(OUTPUT ${VID_PALETTE_PKG} ${VID_PALETTE_H}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${VID_PALETTE_GEN}
        --emit-sv ${VID_PALETTE_PKG} --emit-h ${VID_PALETTE_H}
    DEPENDS ${VID_PALETTE_GEN} ${RP6502_SRC}/vga/term/color.c
    COMMENT "Generating the vid palette ROM"
    VERBATIM)
add_custom_target(vid_palette_rom ALL DEPENDS ${VID_PALETTE_PKG} ${VID_PALETTE_H})

# The PSG's sine table, from aud_init's runtime formula.
set(AUD_SINE_GEN ${RP6502_SRC}/fpga/codegen/aud_sine_gen.py)
set(AUD_SINE_PKG ${CMAKE_CURRENT_BINARY_DIR}/aud_sine_pkg.sv)
set(AUD_SINE_H ${CMAKE_CURRENT_BINARY_DIR}/aud_sine_tables.h)
execute_process(
    COMMAND python3 ${AUD_SINE_GEN} --emit-sv ${AUD_SINE_PKG} --emit-h ${AUD_SINE_H}
    RESULT_VARIABLE AUD_SINE_GEN_RC)
if(AUD_SINE_GEN_RC)
    message(FATAL_ERROR "aud_sine_gen.py failed")
endif()
add_custom_command(OUTPUT ${AUD_SINE_PKG} ${AUD_SINE_H}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${AUD_SINE_GEN}
        --emit-sv ${AUD_SINE_PKG} --emit-h ${AUD_SINE_H}
    DEPENDS ${AUD_SINE_GEN}
    COMMENT "Generating the aud sine ROM"
    VERBATIM)
add_custom_target(aud_sine_rom ALL DEPENDS ${AUD_SINE_PKG} ${AUD_SINE_H})

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
    INCLUDES ${CMAKE_CURRENT_BINARY_DIR} ${RP6502_SRC}
    LIBS fpga_oracle)
add_dependencies(test_font vid_font_rom vid_palette_rom aud_sine_rom)

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
        ${RP6502_FPGA_RTL}/mem/xram64k.sv
        ${RP6502_FPGA_RTL}/ria/ria_regs.sv
        ${RP6502_FPGA_RTL}/rv/rv_soc.sv
        ${RP6502_FPGA_RTL}/vid/vid_timing.sv
        ${AUD_SINE_PKG}
        ${RP6502_FPGA_RTL}/aud/aud_psg.sv
        ${RP6502_FPGA_RTL}/aud/aud_bel.sv
        ${VID_PALETTE_PKG}
        ${RP6502_FPGA_RTL}/vid/vid_font.sv
        ${RP6502_FPGA_RTL}/vid/vid_mode.sv
        ${RP6502_FPGA_RTL}/vid/vid_mode1.sv
        ${RP6502_FPGA_RTL}/vid/vid_mode2.sv
        ${RP6502_FPGA_RTL}/vid/vid_mode3.sv
        ${RP6502_FPGA_RTL}/vid/vid_mode4.sv
        ${RP6502_FPGA_RTL}/vid/vid_mode5.sv
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
        # The firmware's own headers carry the hardware's addresses, so a
        # window that moves has to rebuild the image that writes to it.
        file(GLOB SW_HEADERS ${RP6502_SRC}/fpga/sw/*.h)
        set(SW_SOURCES
            ${SW_SRC}/crt0.S ${SW_SRC}/main.c ${SW_SRC}/aud.c
            ${SW_SRC}/com.c ${SW_SRC}/font.c ${SW_SRC}/kbd.c ${SW_SRC}/mem.c
            ${SW_SRC}/pix.c
            ${SW_SRC}/rand.c ${SW_SRC}/rom.c ${SW_SRC}/time.c
            ${SW_SRC}/trap.c ${SW_SRC}/vga.c ${SW_SRC}/vid.c
            ${RP6502_SRC}/ria/api/api.c
            ${RP6502_SRC}/ria/api/std.c
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
                -I ${CMAKE_CURRENT_BINARY_DIR}
                -I ${RP6502_SRC}/emu/host/pico
                -I ${RP6502_VENDOR}
                "-DPICO_PROGRAM_NAME=\"RP6502-FPGA\""
                -T ${SW_SRC}/link.ld -Wl,--no-warn-rwx-segments
                -o ${CMAKE_CURRENT_BINARY_DIR}/sw.elf
                ${SW_SOURCES}
            COMMAND ${RISCV_OBJCOPY} -O binary
                ${CMAKE_CURRENT_BINARY_DIR}/sw.elf ${SW_BIN}
            DEPENDS ${SW_SOURCES} ${SW_HEADERS} ${VID_FONT_ASSET_H}
                ${SW_SRC}/link.ld
            COMMENT "Cross-compiling the soft CPU firmware"
            VERBATIM)
        add_custom_target(sw_bin DEPENDS ${SW_BIN})

        # The real boot path: no backdoor into the 6502's world, only the
        # firmware image, the way a bitstream carries it.
        rp6502_add_test(boot
            SOURCES test_boot.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
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
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
            TIMEOUT 120)
        add_dependencies(test_compare sw_bin cpu65_rom)
        verilate(test_compare
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # The M3 frame test: one banner ROM, two machines, pixel-exact.
        rp6502_add_test(banner
            SOURCES test_banner.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
            TIMEOUT 300)
        add_dependencies(test_banner sw_bin cpu65_rom vid_font_rom)
        verilate(test_banner
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # RW0/RW1 steps, wraps, and defaults against the oracle.
        rp6502_add_test(rw
            SOURCES test_rw.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
            TIMEOUT 120)
        add_dependencies(test_rw sw_bin cpu65_rom vid_font_rom)
        verilate(test_rw
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # Op 0x01 dispatch against the oracle, and the RTL program it lands.
        rp6502_add_test(xreg
            SOURCES test_xreg.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
            TIMEOUT 120)
        add_dependencies(test_xreg sw_bin cpu65_rom vid_font_rom)
        verilate(test_xreg
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # Mode 1 character cells, per format, pixel-exact vs the oracle.
        rp6502_add_test(mode1
            SOURCES test_mode1.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 300)
        add_dependencies(test_mode1 sw_bin cpu65_rom vid_font_rom vid_palette_rom)
        verilate(test_mode1
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # Mode 2 tile maps, per depth per tile size, trims, wraps, and the
        # composite stacking all three planes, pixel-exact vs the oracle.
        rp6502_add_test(mode2
            SOURCES test_mode2.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 300)
        add_dependencies(test_mode2 sw_bin cpu65_rom vid_font_rom vid_palette_rom)
        verilate(test_mode2
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # Mode 4 sprites: raw sixteen-bit texels and opacity metadata,
        # pixel-exact vs the oracle.
        rp6502_add_test(mode4
            SOURCES test_mode4.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 300)
        add_dependencies(test_mode4 sw_bin cpu65_rom vid_font_rom vid_palette_rom)
        verilate(test_mode4
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # Mode 5 sprites: the foreground walk across planes, clips,
        # per-sprite palettes, pixel-exact vs the oracle.
        rp6502_add_test(mode5
            SOURCES test_mode5.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 300)
        add_dependencies(test_mode5 sw_bin cpu65_rom vid_font_rom vid_palette_rom)
        verilate(test_mode5
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # Mode 3 bitmaps, per depth per canvas, pixel-exact vs the oracle.
        rp6502_add_test(mode3
            SOURCES test_mode3.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 300)
        add_dependencies(test_mode3 sw_bin cpu65_rom vid_font_rom vid_palette_rom)
        verilate(test_mode3
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # XRAM records through the staged loader, judged by the oracle.
        rp6502_add_test(xram
            SOURCES test_xram.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
            TIMEOUT 120)
        add_dependencies(test_xram sw_bin cpu65_rom vid_font_rom)
        verilate(test_xram
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # The M3 exit test: a scripted session settled and pixel-exact,
        # plus the RTL-relative blink phase check.
        rp6502_add_test(session
            SOURCES test_session.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
            TIMEOUT 300)
        add_dependencies(test_session sw_bin cpu65_rom vid_font_rom)
        verilate(test_session
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # furelise.rp6502 on the whole machine — the xreg dispatch, the
        # gate snoop, the rotor fetch, and the 24 kHz grid against
        # 60 Hz — is bypassed for now. Thirty-five of its frames are the
        # tracker's own lead-in before a note sounds, which makes it the
        # slowest test in the suite by a wide margin for what it proves.
        # A faster machine-level audio test covering both the PSG and
        # the OPL replaces it; test_furelise.cpp stays in the tree until
        # that lands. The PSG's own correctness is not at stake here —
        # test_psg holds it bit-exact against ria/aud/psg.c in lockstep.

        # The .rp6502 loader against the staged window, the testbench
        # playing the platform bridge.
        rp6502_add_test(rom
            SOURCES test_rom.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
            TIMEOUT 120)
        add_dependencies(test_rom sw_bin cpu65_rom)
        verilate(test_rom
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )
        rp6502_add_test(budget
            SOURCES test_budget.cpp
            LIBS fpga_oracle
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
                ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 600)
        add_dependencies(test_budget sw_bin cpu65_rom vid_font_rom vid_palette_rom)
        verilate(test_budget
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        rp6502_add_test(fontstore
            SOURCES test_fontstore.cpp
            LIBS fpga_oracle
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
            TIMEOUT 300)
        add_dependencies(test_fontstore sw_bin cpu65_rom vid_font_rom)
        verilate(test_fontstore
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

    # --- The platform FIFO under hostile clocking ---
    rp6502_add_test(fifo
        SOURCES test_fifo.cpp
        TIMEOUT 120)
    verilate(test_fifo
        SOURCES ${RP6502_FPGA_POCKET}/pocket_fifo.sv
        TOP_MODULE pocket_fifo
        PREFIX Vpocket_fifo
        VERILATOR_ARGS -Wall --assert -O3
    )

    # --- The scaler-side raster from a replayed beam ---
    rp6502_add_test(pvideo
        SOURCES test_pvideo.cpp
        TIMEOUT 120)
    verilate(test_pvideo
        SOURCES ${RP6502_FPGA_POCKET}/pocket_fifo.sv
            ${RP6502_FPGA_POCKET}/pocket_video.sv
        TOP_MODULE pocket_video
        PREFIX Vpocket_video
        VERILATOR_ARGS -Wall --assert -O3
    )

    # --- The codec-side I2S from replayed samples ---
    rp6502_add_test(pi2s
        SOURCES test_pi2s.cpp
        TIMEOUT 120)
    verilate(test_pi2s
        SOURCES ${RP6502_FPGA_POCKET}/pocket_fifo.sv
            ${RP6502_FPGA_POCKET}/pocket_i2s.sv
        TOP_MODULE pocket_i2s
        PREFIX Vpocket_i2s
        VERILATOR_ARGS -Wall --assert -O3
    )

    # --- The staging SDRAM controller against a behavioral chip ---
    rp6502_add_test(psdram
        SOURCES test_psdram.cpp
        TIMEOUT 300)
    verilate(test_psdram
        SOURCES ${RP6502_FPGA_POCKET}/pocket_sdram.sv
            ${CMAKE_CURRENT_LIST_DIR}/sdram_model.sv
            ${CMAKE_CURRENT_LIST_DIR}/tb_psdram.sv
        TOP_MODULE tb_psdram
        PREFIX Vtb_psdram
        VERILATOR_ARGS -Wall --assert -O3
    )

    # --- The bridge chain, played like the host ---
    rp6502_add_test(pbridge
        SOURCES test_pbridge.cpp
        TIMEOUT 300)
    verilate(test_pbridge
        SOURCES ${RP6502_FPGA_POCKET}/pocket_fifo.sv
            ${RP6502_FPGA_POCKET}/pocket_sdram.sv
            ${RP6502_FPGA_POCKET}/pocket_bridge.sv
            ${CMAKE_CURRENT_LIST_DIR}/sdram_model.sv
            ${CMAKE_CURRENT_LIST_DIR}/tb_pbridge.sv
        TOP_MODULE tb_pbridge
        PREFIX Vtb_pbridge
        VERILATOR_ARGS -Wall --assert -O3
    )

    # --- The whole Pocket stack, frame-exact against the oracle ---
    if(RISCV_GCC AND RISCV_OBJCOPY)
        rp6502_add_test(pocket
            SOURCES test_pocket.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            LIBS fpga_oracle
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 600)
        add_dependencies(test_pocket sw_bin cpu65_rom vid_font_rom
            vid_palette_rom)
        verilate(test_pocket
            SOURCES ${RP6502_MACHINE_SOURCES}
                ${RP6502_FPGA_POCKET}/pocket_fifo.sv
                ${RP6502_FPGA_POCKET}/pocket_video.sv
                ${RP6502_FPGA_POCKET}/pocket_i2s.sv
                ${RP6502_FPGA_POCKET}/pocket_sdram.sv
                ${RP6502_FPGA_POCKET}/pocket_bridge.sv
                ${RP6502_FPGA_POCKET}/pocket_core.sv
                ${CMAKE_CURRENT_LIST_DIR}/sdram_model.sv
                ${CMAKE_CURRENT_LIST_DIR}/tb_pocket.sv
            TOP_MODULE tb_pocket
            PREFIX Vtb_pocket
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )
    endif()

    # --- The PSG in RTL, against ria/aud/psg.c in lockstep ---
    # The bit-exact audio proof: the vendored DSP over flat shims and the
    # verilated engine consume the same XRAM image and gate schedule.
    rp6502_add_test(psg
        SOURCES test_psg.cpp psg_shim.c
            ${RP6502_SRC}/ria/aud/psg.c ${RP6502_SRC}/ria/aud/bel.c
        INCLUDES ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_BINARY_DIR}
            ${RP6502_SRC} ${RP6502_SRC}/emu/host/pico
        TIMEOUT 300)
    add_dependencies(test_psg aud_sine_rom)
    verilate(test_psg
        SOURCES ${AUD_SINE_PKG} ${RP6502_FPGA_RTL}/aud/aud_psg.sv
            ${RP6502_FPGA_RTL}/aud/aud_bel.sv
        TOP_MODULE aud_psg
        PREFIX Vaud_psg
        VERILATOR_ARGS -Wall --assert -O3 -GTICKS_PER_SAMPLE=1200
    )

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

# --- Synthesis: the same RTL through Quartus, for area and timing ---
# Not part of the test suite — `cmake --build . --target synth` writes a
# project into the build tree and runs the fitter and the analyzer. The
# source list is the verilated one, so the thing measured is the thing
# tested, and the generated packages come from this build rather than a
# copy that can drift.
find_program(QUARTUS_MAP quartus_map HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_FIT quartus_fit HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
find_program(QUARTUS_STA quartus_sta HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
if(QUARTUS_MAP AND QUARTUS_FIT AND QUARTUS_STA)
    set(SYNTH_DIR ${CMAKE_CURRENT_BINARY_DIR}/synth)
    set(SYNTH_QSF ${SYNTH_DIR}/rp6502.qsf)
    set(SYNTH_SDC ${RP6502_SRC}/fpga/platform/pocket/quartus/rp6502.sdc)
    set(SYNTH_LINES
        "set_global_assignment -name FAMILY \"Cyclone V\""
        "set_global_assignment -name DEVICE 5CEBA4F23C8"
        "set_global_assignment -name TOP_LEVEL_ENTITY rp6502"
        "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files"
        "set_global_assignment -name SDC_FILE ${SYNTH_SDC}"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl/arith")
    foreach(src ${RP6502_MACHINE_SOURCES})
        if(src MATCHES "\\.sv$")
            list(APPEND SYNTH_LINES
                "set_global_assignment -name SYSTEMVERILOG_FILE ${src}")
        endif()
    endforeach()
    string(REPLACE ";" "\n" SYNTH_QSF_TEXT "${SYNTH_LINES}")
    file(MAKE_DIRECTORY ${SYNTH_DIR})
    file(WRITE ${SYNTH_QSF} "${SYNTH_QSF_TEXT}\n")
    file(WRITE ${SYNTH_DIR}/rp6502.qpf
        "PROJECT_REVISION = \"rp6502\"\n")
    add_custom_target(synth
        COMMAND ${QUARTUS_MAP} rp6502
        COMMAND ${QUARTUS_FIT} rp6502
        COMMAND ${QUARTUS_STA} rp6502
        WORKING_DIRECTORY ${SYNTH_DIR}
        DEPENDS cpu65_rom vid_font_rom vid_palette_rom aud_sine_rom
        COMMENT "Synthesizing the machine for the Pocket's Cyclone V"
        VERBATIM)
endif()
