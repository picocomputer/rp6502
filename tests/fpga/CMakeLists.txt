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

# Two audio programs that make one note and leave the console alone, so
# the machine's own diagnostics stay readable while a device is driven.
# test_aud runs these same files, which is what keeps a note that sounds
# on hardware and a note the simulation asserts from drifting apart.
set(AUD_ROM_GEN ${RP6502_SRC}/fpga/codegen/aud_rom_gen.py)
set(AUD_ROM_PSG ${CMAKE_CURRENT_BINARY_DIR}/psg.rp6502)
set(AUD_ROM_OPL ${CMAKE_CURRENT_BINARY_DIR}/opl.rp6502)
execute_process(
    COMMAND python3 ${AUD_ROM_GEN} --emit-psg ${AUD_ROM_PSG}
        --emit-opl ${AUD_ROM_OPL}
    RESULT_VARIABLE AUD_ROM_GEN_RC)
if(AUD_ROM_GEN_RC)
    message(FATAL_ERROR "aud_rom_gen.py failed")
endif()
add_custom_command(OUTPUT ${AUD_ROM_PSG} ${AUD_ROM_OPL}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${AUD_ROM_GEN}
        --emit-psg ${AUD_ROM_PSG} --emit-opl ${AUD_ROM_OPL}
    DEPENDS ${AUD_ROM_GEN}
    COMMENT "Generating the audio bring-up ROMs"
    VERBATIM)
add_custom_target(aud_roms ALL DEPENDS ${AUD_ROM_PSG} ${AUD_ROM_OPL})

# The OEM code page tables. This machine cannot link them in, so it gets
# the binary and loads it into the staging store beside the fonts.
set(OEMCP_GEN ${RP6502_SRC}/fpga/codegen/oem_table_gen.py)
set(OEMCP_SRC ${RP6502_VENDOR}/fatfs/ffunicode.c)
set(OEMCP_BIN ${CMAKE_CURRENT_BINARY_DIR}/oemcp.bin)
execute_process(
    COMMAND python3 ${OEMCP_GEN} --ffunicode ${OEMCP_SRC}
        --emit-bin ${OEMCP_BIN}
    RESULT_VARIABLE OEMCP_GEN_RC)
if(OEMCP_GEN_RC)
    message(FATAL_ERROR "oem_table_gen.py failed")
endif()
add_custom_command(OUTPUT ${OEMCP_BIN}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${OEMCP_GEN}
        --ffunicode ${OEMCP_SRC} --emit-bin ${OEMCP_BIN}
    DEPENDS ${OEMCP_GEN} ${OEMCP_SRC}
    COMMENT "Generating the OEM code page tables"
    VERBATIM)
add_custom_target(oemcp_bin ALL DEPENDS ${OEMCP_BIN})

# The file round trip, generated the same way and shipped the same way.
set(FILE_ROM_GEN ${RP6502_SRC}/fpga/codegen/file_rom_gen.py)
set(FILE_ROM ${CMAKE_CURRENT_BINARY_DIR}/file.rp6502)
execute_process(
    COMMAND python3 ${FILE_ROM_GEN} --emit ${FILE_ROM}
    RESULT_VARIABLE FILE_ROM_GEN_RC)
if(FILE_ROM_GEN_RC)
    message(FATAL_ERROR "file_rom_gen.py failed")
endif()
add_custom_command(OUTPUT ${FILE_ROM}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${FILE_ROM_GEN} --emit ${FILE_ROM}
    DEPENDS ${FILE_ROM_GEN}
    COMMENT "Generating the file round-trip ROM"
    VERBATIM)
add_custom_target(file_rom ALL DEPENDS ${FILE_ROM})

# The same round trip past the transfer window. It ships but is not a
# test: what it exists to ask — whether the Pocket's resize keeps what
# was already in the file — has no answer in simulation, because the
# bench answers the way we assumed.
set(BIGFILE_ROM_GEN ${RP6502_SRC}/fpga/codegen/bigfile_rom_gen.py)
set(BIGFILE_ROM ${CMAKE_CURRENT_BINARY_DIR}/bigfile.rp6502)
execute_process(
    COMMAND python3 ${BIGFILE_ROM_GEN} --emit ${BIGFILE_ROM}
    RESULT_VARIABLE BIGFILE_ROM_GEN_RC)
if(BIGFILE_ROM_GEN_RC)
    message(FATAL_ERROR "bigfile_rom_gen.py failed")
endif()
add_custom_command(OUTPUT ${BIGFILE_ROM}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${BIGFILE_ROM_GEN}
        --emit ${BIGFILE_ROM}
    DEPENDS ${BIGFILE_ROM_GEN}
    COMMENT "Generating the multi-chunk file ROM"
    VERBATIM)
add_custom_target(bigfile_rom ALL DEPENDS ${BIGFILE_ROM})

# The create path has never worked on hardware and the name turned out
# not to matter. This walks a list of names in one boot so the next
# guess costs a card copy instead of a fit.
set(PROBE_ROM_GEN ${RP6502_SRC}/fpga/codegen/probe_rom_gen.py)
set(PROBE_ROM ${CMAKE_CURRENT_BINARY_DIR}/probe.rp6502)
execute_process(
    COMMAND python3 ${PROBE_ROM_GEN} --emit ${PROBE_ROM}
    WORKING_DIRECTORY ${RP6502_SRC}/fpga/codegen
    RESULT_VARIABLE PROBE_ROM_GEN_RC)
if(PROBE_ROM_GEN_RC)
    message(FATAL_ERROR "probe_rom_gen.py failed")
endif()
add_custom_command(OUTPUT ${PROBE_ROM}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${PROBE_ROM_GEN}
        --emit ${PROBE_ROM}
    WORKING_DIRECTORY ${RP6502_SRC}/fpga/codegen
    DEPENDS ${PROBE_ROM_GEN} ${BIGFILE_ROM_GEN}
    COMMENT "Generating the open-file probe ROM"
    VERBATIM)
add_custom_target(probe_rom ALL DEPENDS ${PROBE_ROM})

# The whole drive in one boot: twenty-six checks the machine decides
# for itself. It runs here against the bench's host as well as on the
# card, so a bug in the ROM is found before a photograph is.
set(FSTEST_ROM_GEN ${RP6502_SRC}/fpga/codegen/fstest_rom_gen.py)
set(FSTEST_ROM ${CMAKE_CURRENT_BINARY_DIR}/fstest.rp6502)
execute_process(
    COMMAND python3 ${FSTEST_ROM_GEN} --emit ${FSTEST_ROM}
    WORKING_DIRECTORY ${RP6502_SRC}/fpga/codegen
    RESULT_VARIABLE FSTEST_ROM_GEN_RC)
if(FSTEST_ROM_GEN_RC)
    message(FATAL_ERROR "fstest_rom_gen.py failed")
endif()
add_custom_command(OUTPUT ${FSTEST_ROM}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${FSTEST_ROM_GEN}
        --emit ${FSTEST_ROM}
    WORKING_DIRECTORY ${RP6502_SRC}/fpga/codegen
    DEPENDS ${FSTEST_ROM_GEN} ${BIGFILE_ROM_GEN}
    COMMENT "Generating the filesystem conformance ROM"
    VERBATIM)
add_custom_target(fstest_rom ALL DEPENDS ${FSTEST_ROM})

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
        ${CMAKE_CURRENT_LIST_DIR}/opl2.vlt
        ${RP6502_VENDOR}/opl2_fpga_rp6502/opl2_pkg.sv
        ${RP6502_VENDOR}/opl2_fpga_rp6502/mem_single_bank.sv
        ${RP6502_VENDOR}/opl2_fpga_rp6502/mem_simple_dual_port.sv
        ${RP6502_VENDOR}/opl2_fpga_rp6502/trick_sw_detection.sv)
    foreach(dir top_level channels operator timers host_if misc clks)
        file(GLOB _opl_dir_src ${OPL2_DIR}/${dir}/src/*.sv ${OPL2_DIR}/${dir}/src/*.v)
        list(FILTER _opl_dir_src EXCLUDE REGEX
            "/(i2s|mem_single_bank|mem_simple_dual_port|trick_sw_detection)\\.sv$")
        list(APPEND OPL2_SOURCES ${_opl_dir_src})
    endforeach()

    set(RP6502_MACHINE_SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/hazard3.vlt
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
        ${VID_PALETTE_PKG}
        ${RP6502_FPGA_RTL}/vid/vid_font.sv
        ${RP6502_FPGA_RTL}/vid/vid_palram.sv
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

    # The OPL2 device: the pointer gates it, a poke reaches the chip.
    rp6502_add_test(opl
        SOURCES test_opl.cpp
        INCLUDES ${CMAKE_CURRENT_LIST_DIR}
        TIMEOUT 300)
    verilate(test_opl
        SOURCES ${OPL2_SOURCES} ${RP6502_FPGA_RTL}/aud/aud_bel.sv
            ${RP6502_FPGA_RTL}/aud/aud_opl.sv
        TOP_MODULE aud_opl
        PREFIX Vaud_opl
        VERILATOR_ARGS -Wall --assert -O3
    )

    # The soft CPU's microsecond clock against clk_sys wall time.
    rp6502_add_test(mtime
        SOURCES test_mtime.cpp
        INCLUDES ${CMAKE_CURRENT_LIST_DIR}
        TIMEOUT 120)
    add_dependencies(test_mtime cpu65_rom)
    verilate(test_mtime
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
                OEMCP_BIN="${OEMCP_BIN}"
            TIMEOUT 900)
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
                OEMCP_BIN="${OEMCP_BIN}"
            TIMEOUT 900)
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
                OEMCP_BIN="${OEMCP_BIN}"
            TIMEOUT 900)
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
                OEMCP_BIN="${OEMCP_BIN}"
            TIMEOUT 900)
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
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
                OEMCP_BIN="${OEMCP_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 900)
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
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
                OEMCP_BIN="${OEMCP_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 900)
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
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
                OEMCP_BIN="${OEMCP_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 900)
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
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
                OEMCP_BIN="${OEMCP_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 900)
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
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
                OEMCP_BIN="${OEMCP_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 900)
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
                OEMCP_BIN="${OEMCP_BIN}"
            TIMEOUT 900)
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
                OEMCP_BIN="${OEMCP_BIN}"
            TIMEOUT 900)
        add_dependencies(test_session sw_bin cpu65_rom vid_font_rom)
        verilate(test_session
            SOURCES ${RP6502_MACHINE_SOURCES}
            TOP_MODULE rp6502
            PREFIX Vrp6502
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # Both engines end to end: a program asks for the device, pours
        # its registers in through RW0, and the machine makes a noise
        # inside a couple of frames. This is the plumbing test — the
        # xreg dispatch, the write snoop, and the sample grid — not a
        # waveform one; test_psg holds the PSG bit-exact against
        # ria/aud/psg.c, and the OPL2 has no emulator to agree with.
        rp6502_add_test(aud
            SOURCES test_aud.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
                OEMCP_BIN="${OEMCP_BIN}" AUD_ROM_PSG="${AUD_ROM_PSG}" AUD_ROM_OPL="${AUD_ROM_OPL}"
            TIMEOUT 600)
        add_dependencies(test_aud sw_bin cpu65_rom aud_roms)
        verilate(test_aud
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
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
                OEMCP_BIN="${OEMCP_BIN}"
            TIMEOUT 900)
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
                OEMCP_BIN="${OEMCP_BIN}"
                ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 1200)
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
                OEMCP_BIN="${OEMCP_BIN}"
            TIMEOUT 900)
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

    # --- Every whole kilohertz, exactly ---
    rp6502_add_test(phi2
        SOURCES test_phi2.cpp
        TIMEOUT 120)
    verilate(test_phi2
        SOURCES ${RP6502_SRC}/fpga/rtl/cpu65/phi2_div.sv
        TOP_MODULE phi2_div
        PREFIX Vphi2_div
        VERILATOR_ARGS -Wall --assert -O3
    )

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
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
                OEMCP_BIN="${OEMCP_BIN}" ROMS_DIR="${RP6502_TEST_ROMS}"
            TIMEOUT 1200)
        add_dependencies(test_pocket sw_bin cpu65_rom vid_font_rom
            vid_palette_rom oemcp_bin)
        verilate(test_pocket
            SOURCES ${RP6502_MACHINE_SOURCES}
                ${RP6502_FPGA_POCKET}/pocket_fifo.sv
                ${RP6502_FPGA_POCKET}/pocket_video.sv
                ${RP6502_FPGA_POCKET}/pocket_i2s.sv
                ${RP6502_FPGA_POCKET}/pocket_sdram.sv
                ${RP6502_FPGA_POCKET}/pocket_bridge.sv
                ${RP6502_FPGA_POCKET}/pocket_file.sv
                ${RP6502_FPGA_POCKET}/pocket_core.sv
                ${CMAKE_CURRENT_LIST_DIR}/sdram_model.sv
                ${CMAKE_CURRENT_LIST_DIR}/tb_pocket.sv
            TOP_MODULE tb_pocket
            PREFIX Vtb_pocket
            INCLUDE_DIRS ${RP6502_VENDOR}/hazard3/hdl
            VERILATOR_ARGS -Wall --assert -O3 ${RP6502_MACHINE_VERILATOR_ARGS}
        )

        # --- The file bridge, with the bench playing the host ---
        rp6502_add_test(pfile
            SOURCES test_pfile.cpp
            INCLUDES ${CMAKE_CURRENT_LIST_DIR}
            DEFS SW_BIN="${SW_BIN}" FONTS_BIN="${VID_FONT_BIN}"
                OEMCP_BIN="${OEMCP_BIN}" FILE_ROM="${FILE_ROM}"
                FSTEST_ROM="${FSTEST_ROM}"
            TIMEOUT 1200)
        add_dependencies(test_pfile sw_bin cpu65_rom vid_font_rom
            vid_palette_rom file_rom fstest_rom oemcp_bin)
        verilate(test_pfile
            SOURCES ${RP6502_MACHINE_SOURCES}
                ${RP6502_FPGA_POCKET}/pocket_fifo.sv
                ${RP6502_FPGA_POCKET}/pocket_video.sv
                ${RP6502_FPGA_POCKET}/pocket_i2s.sv
                ${RP6502_FPGA_POCKET}/pocket_sdram.sv
                ${RP6502_FPGA_POCKET}/pocket_bridge.sv
                ${RP6502_FPGA_POCKET}/pocket_file.sv
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
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl/arith"
        # A shift register the fitter recognises becomes an M10K, and the
        # ones here are short enough that a block is a poor trade: three
        # blocks go back for a handful of ALMs. Blocks are what this
        # device is short of, not logic.
        "set_global_assignment -name AUTO_SHIFT_REGISTER_RECOGNITION OFF"
        # The machine's own ports outnumber the package's pins — the host
        # window alone is a hundred of them — and this target exists to
        # measure area, not to be bound to pads.
        "set_instance_assignment -name VIRTUAL_PIN ON -to *")
    foreach(src ${RP6502_MACHINE_SOURCES})
        if(src MATCHES "\\.sv$")
            list(APPEND SYNTH_LINES
                "set_global_assignment -name SYSTEMVERILOG_FILE ${src}")
        elseif(src MATCHES "\\.v$")
            list(APPEND SYNTH_LINES
                "set_global_assignment -name VERILOG_FILE ${src}")
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

    # And the same again with the Pocket's own layer on top, which is
    # what actually has to fit: the machine alone leaves out the bridge,
    # the SDRAM controller, the I2S and the video crossing. The ports
    # are virtual because pocket_core presents more signals than the
    # package has pins — Analogue's core_top is what binds them to pads,
    # and it is not vendored yet.
    set(PSYNTH_DIR ${CMAKE_CURRENT_BINARY_DIR}/synth_pocket)
    set(PSYNTH_QSF ${PSYNTH_DIR}/pocket.qsf)
    set(PSYNTH_LINES
        "set_global_assignment -name FAMILY \"Cyclone V\""
        "set_global_assignment -name DEVICE 5CEBA4F23C8"
        "set_global_assignment -name TOP_LEVEL_ENTITY pocket_core"
        "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files"
        "set_global_assignment -name SDC_FILE ${SYNTH_SDC}"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl/arith"
        "set_instance_assignment -name VIRTUAL_PIN ON -to *")
    foreach(src ${RP6502_MACHINE_SOURCES})
        if(src MATCHES "\\.sv$")
            list(APPEND PSYNTH_LINES
                "set_global_assignment -name SYSTEMVERILOG_FILE ${src}")
        elseif(src MATCHES "\\.v$")
            list(APPEND PSYNTH_LINES
                "set_global_assignment -name VERILOG_FILE ${src}")
        endif()
    endforeach()
    foreach(src pocket_fifo pocket_video pocket_i2s pocket_sdram
            pocket_bridge pocket_file pocket_core)
        list(APPEND PSYNTH_LINES
            "set_global_assignment -name SYSTEMVERILOG_FILE ${RP6502_FPGA_POCKET}/${src}.sv")
    endforeach()
    string(REPLACE ";" "\n" PSYNTH_QSF_TEXT "${PSYNTH_LINES}")
    file(MAKE_DIRECTORY ${PSYNTH_DIR})
    file(WRITE ${PSYNTH_QSF} "${PSYNTH_QSF_TEXT}\n")
    file(WRITE ${PSYNTH_DIR}/pocket.qpf "PROJECT_REVISION = \"pocket\"\n")
    # The real thing: Analogue's framework on top, their pin assignments,
    # their apf_top as the root, and a bitstream at the end of it. The
    # pins and device come from the vendored project verbatim — only its
    # own source list is dropped, because those files are what we
    # replace.
    set(BS_DIR ${CMAKE_CURRENT_BINARY_DIR}/bitstream)
    set(BS_QSF ${BS_DIR}/rp6502.qsf)
    set(APF ${RP6502_VENDOR}/openfpga/src/fpga)
    # Analogue's framework is a submodule the simulation never needs, so
    # it is often absent — CI checks out only what it builds.
    # The array the firmware is loaded into is sized in the RTL, so read
    # the size from there rather than keeping a second copy in step.
    file(STRINGS ${RP6502_SRC}/fpga/rtl/rv/rv_soc.sv TCM_WORDS_LINE
        REGEX "localparam int TCM_WORDS")
    string(REGEX MATCH "[0-9]+" TCM_WORDS "${TCM_WORDS_LINE}")

    # No firmware, no bitstream. A bitstream without it boots to a soft
    # CPU fetching zeros, which looks like dead hardware and is not.
    if(EXISTS ${APF}/ap_core.qsf AND SW_BIN)
    file(MAKE_DIRECTORY ${BS_DIR})
    file(STRINGS ${APF}/ap_core.qsf BS_TEMPLATE)
    set(BS_LINES "")
    foreach(line ${BS_TEMPLATE})
        if(NOT line MATCHES "_FILE|SEARCH_PATH|SIGNALTAP")
            list(APPEND BS_LINES "${line}")
        endif()
    endforeach()
    list(APPEND BS_LINES
        "set_global_assignment -name GENERATE_RBF_FILE ON"
        # Analogue's template asks for Auto Fit, which stops optimizing
        # the moment timing is met. That is why hold on the soft CPU's
        # crossing into the machine landed at a single picosecond: met,
        # and abandoned there. Standard Fit keeps going, and the margin
        # it finds is what survives the next fit's placement.
        "set_global_assignment -name FITTER_EFFORT \"STANDARD FIT\""
        "set_global_assignment -name SEARCH_PATH ${BS_DIR}"
        "set_global_assignment -name QIP_FILE ${APF}/apf/apf.qip"
        "set_global_assignment -name VERILOG_FILE ${RP6502_VENDOR}/openfpga_rp6502/core_bridge_cmd.v"
        "set_global_assignment -name VERILOG_FILE ${APF}/core/pin_ddio_clk.v"
        "set_global_assignment -name SDC_FILE ${APF}/apf/apf_constraints.sdc"
        "set_global_assignment -name SDC_FILE ${APF}/core/core_constraints.sdc"
        "set_global_assignment -name SYSTEMVERILOG_FILE ${RP6502_SRC}/fpga/platform/pocket/core_top.sv"
        "set_global_assignment -name VERILOG_FILE ${RP6502_SRC}/fpga/platform/pocket/pocket_pll.v"
        "set_global_assignment -name SDC_FILE ${SYNTH_SDC}"
        "set_global_assignment -name SDC_FILE ${RP6502_SRC}/fpga/platform/pocket/quartus/pocket.sdc"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl"
        "set_global_assignment -name SEARCH_PATH ${RP6502_VENDOR}/hazard3/hdl/arith"
        # A shift register the fitter recognises becomes an M10K, and the
        # ones here are short enough that a block is a poor trade: three
        # blocks go back for a handful of ALMs. Blocks are what this
        # device is short of, not logic.
        "set_global_assignment -name AUTO_SHIFT_REGISTER_RECOGNITION OFF")
    foreach(src ${RP6502_MACHINE_SOURCES})
        if(src MATCHES "\\.sv$")
            list(APPEND BS_LINES
                "set_global_assignment -name SYSTEMVERILOG_FILE ${src}")
        elseif(src MATCHES "\\.v$")
            list(APPEND BS_LINES
                "set_global_assignment -name VERILOG_FILE ${src}")
        endif()
    endforeach()
    foreach(src pocket_fifo pocket_video pocket_i2s pocket_sdram
            pocket_bridge pocket_file pocket_bars pocket_dbg pocket_dbglog
            pocket_core)
        list(APPEND BS_LINES
            "set_global_assignment -name SYSTEMVERILOG_FILE ${RP6502_FPGA_POCKET}/${src}.sv")
    endforeach()
    string(REPLACE ";" "\n" BS_QSF_TEXT "${BS_LINES}")
    file(WRITE ${BS_QSF} "${BS_QSF_TEXT}\n")
    file(WRITE ${BS_DIR}/rp6502.qpf "PROJECT_REVISION = \"rp6502\"\n")
    find_program(QUARTUS_ASM quartus_asm HINTS $ENV{HOME}/altera_lite/25.1std/quartus/bin)
    # Everything the SD card needs, assembled. The JSONs, the images and
    # the tree come from dist/ as they are; the bitstream and the glyph
    # asset are built.
    set(PKG_DIR ${CMAKE_CURRENT_BINARY_DIR}/package)
    set(PKG_DIST ${RP6502_SRC}/fpga/platform/pocket/dist)
    add_custom_target(package
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${PKG_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PKG_DIST} ${PKG_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${PKG_DIR}/Assets/rp6502/common
        COMMAND ${CMAKE_COMMAND} -E copy ${VID_FONT_BIN}
            ${PKG_DIR}/Assets/rp6502/common/fonts.bin
        COMMAND ${CMAKE_COMMAND} -E copy ${OEMCP_BIN}
            ${PKG_DIR}/Assets/rp6502/common/oemcp.bin
        COMMAND ${CMAKE_COMMAND} -E copy ${BS_DIR}/bitstream.rbf_r
            ${PKG_DIR}/Cores/Rumbledethumps.RP6502/bitstream.rbf_r
        DEPENDS vid_font_rom
        COMMENT "Assembling the Pocket core package (run `bitstream` first)"
        VERBATIM)

    # The firmware has to be IN the bitstream. Simulation loads it into
    # the verilated arrays from C++, so nothing in the synthesis path
    # ever needed it and for a long time nothing supplied it — the soft
    # CPU came up fetching zeros.
    add_custom_target(bitstream
        COMMAND python3 ${RP6502_SRC}/fpga/codegen/rv_tcm_gen.py
            ${SW_BIN} ${BS_DIR}/sw ${TCM_WORDS}
        COMMAND ${QUARTUS_MAP} rp6502
        COMMAND ${QUARTUS_FIT} rp6502
        COMMAND ${QUARTUS_ASM} rp6502
        COMMAND ${QUARTUS_STA} rp6502
        # No .rbf_r from a fit that did not close. A bitstream that
        # misses timing assembles and runs, and only stops running when
        # the part is warm or the fitter's luck turns.
        COMMAND python3 ${RP6502_SRC}/fpga/codegen/sta_gate.py
            ${BS_DIR}/output_files/rp6502.sta.rpt
        COMMAND python3 ${RP6502_SRC}/fpga/codegen/rbf_r_gen.py
            ${BS_DIR}/output_files/rp6502.rbf
            ${BS_DIR}/bitstream.rbf_r
        WORKING_DIRECTORY ${BS_DIR}
        DEPENDS sw_bin cpu65_rom vid_font_rom vid_palette_rom aud_sine_rom
        COMMENT "Building the Pocket bitstream"
        VERBATIM)
    elseif(NOT EXISTS ${APF}/ap_core.qsf)
        message(STATUS
            "vendor/openfpga absent - no bitstream target. "
            "git submodule update --init vendor/openfpga")
    else()
        message(STATUS
            "no RISC-V toolchain - no bitstream target. "
            "apt install gcc-riscv64-unknown-elf")
    endif()

    add_custom_target(synth_pocket
        COMMAND ${QUARTUS_MAP} pocket
        COMMAND ${QUARTUS_FIT} pocket
        COMMAND ${QUARTUS_STA} pocket
        WORKING_DIRECTORY ${PSYNTH_DIR}
        DEPENDS cpu65_rom vid_font_rom vid_palette_rom aud_sine_rom
        COMMENT "Synthesizing the whole Pocket core"
        VERBATIM)
endif()
