# The machine, as a list of files, and the firmware its soft CPU runs.
#
# Both halves of this tree need these and neither needs a simulator to have
# them: the verilated model is built from this list, and so is every Quartus
# project. Guarding them behind verilator_FOUND would mean no bitstream
# without Verilator installed, which CI's bitstream runner does not have.
#
# RP6502_SOFT_CPU  the RISC-V toolchain is present, so anything that needs a
#                  booted soft CPU can be registered.

# Where the Pocket's own SystemVerilog lives. Set before assets.cmake, which
# names it in the stage_map gate.
set(RP6502_HOST_POCKET ${RP6502_SRC}/host/pocket/core)

include(${CMAKE_CURRENT_LIST_DIR}/assets.cmake)

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
    ${RP6502_VENDOR}/opl2_fpga_rp6502/opl2_pkg.sv
    ${OPL2_LUT_PKG}
    ${RP6502_VENDOR}/opl2_fpga_rp6502/opl2_lut_rom.sv
    ${RP6502_VENDOR}/opl2_fpga_rp6502/phase_generator.sv
    ${RP6502_VENDOR}/opl2_fpga_rp6502/mem_single_bank.sv
    ${RP6502_VENDOR}/opl2_fpga_rp6502/mem_simple_dual_port.sv
    ${RP6502_VENDOR}/opl2_fpga_rp6502/trick_sw_detection.sv
    ${RP6502_VENDOR}/opl2_fpga_rp6502/afifo.v)
foreach(dir top_level channels operator timers host_if misc clks)
    file(GLOB _opl_dir_src ${OPL2_DIR}/${dir}/src/*.sv ${OPL2_DIR}/${dir}/src/*.v)
    list(FILTER _opl_dir_src EXCLUDE REGEX
        "/(i2s|mem_single_bank|mem_simple_dual_port|trick_sw_detection|phase_generator|opl2_log_sine_lut|opl2_exp_lut)\\.sv$|/afifo\\.v$")
    list(APPEND OPL2_SOURCES ${_opl_dir_src})
endforeach()

set(RP6502_MACHINE_SOURCES
    ${RP6502_VENDOR}/hazard3_rp6502/hazard3_regfile_1w2r.v
    ${OPL2_SOURCES}
    ${W65C02_ROM}
    ${RP6502_SRC}/core/machine/rp6502_pkg.sv
    ${RP6502_SRC}/core/wdc/w65c02.sv
    ${RP6502_SRC}/core/wdc/w65c22.sv
    ${RP6502_SRC}/core/mem/sram64k.sv
    ${RP6502_SRC}/core/mem/xram64k.sv
    ${RP6502_SRC}/core/ria/phi2_div.sv
    ${RP6502_SRC}/core/ria/ria_regs.sv
    ${RP6502_SRC}/core/rv/rv_soc.sv
    ${RP6502_SRC}/core/vga/vid_timing.sv
    ${AUD_SINE_PKG}
    ${RP6502_SRC}/core/aud/aud_psg.sv
    ${RP6502_SRC}/core/aud/aud_opl.sv
    ${RSMP_COEF_PKG}
    ${RP6502_SRC}/core/aud/aud_rsmp.sv
    ${VID_PALETTE_PKG}
    ${RP6502_SRC}/core/vga/vid_font.sv
    ${RP6502_SRC}/core/vga/vid_palram.sv
    ${RP6502_SRC}/core/vga/vid_pixtail.sv
    ${RP6502_SRC}/core/vga/vid_sched.sv
    ${RP6502_SRC}/core/vga/vid_fill.sv
    ${RP6502_SRC}/core/vga/vid_mode.sv
    ${RP6502_SRC}/core/vga/vid_mode1.sv
    ${RP6502_SRC}/core/vga/vid_mode2.sv
    ${RP6502_SRC}/core/vga/vid_mode3.sv
    ${RP6502_SRC}/core/vga/vid_mode4.sv
    ${RP6502_SRC}/core/vga/vid_mode5.sv
    ${RP6502_SRC}/core/vga/vid_palcache.sv
    ${RP6502_SRC}/core/vga/vid_sbuf.sv
    ${RP6502_SRC}/core/vga/vid_sprite.sv
    ${RP6502_SRC}/core/vga/vid_prog.sv
    ${RP6502_SRC}/core/vga/vid_mode0.sv
    ${RP6502_SRC}/core/vga/vid_compose.sv
    ${RP6502_SRC}/core/machine/sst_engine.sv
    ${RP6502_SRC}/core/machine/rp6502.sv)
# Verilator elaborates while cmake configures, so an unresolved module here
# is a configure error, not a build one. Nothing recursive: Hazard3 has six
# submodules of its own and this tree reads none of them.
rp6502_submodule(vendor/hazard3 SENTINEL hdl/hazard3_core.v
    WANTS "the soft CPU")
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
    set(SW_SRC ${RP6502_SRC}/host/pocket/sw)
    set(SW_BIN ${RP6502_ASSETS}/sw.bin)
    # The firmware's own headers carry the hardware's addresses, so a window
    # that moves has to rebuild the image that writes to it; the rest are here
    # because this image compiles core sources. Broader than the include list
    # needs, which costs one gcc run and nothing re-verilates behind it.
    # CONFIGURE_DEPENDS because a plain GLOB is evaluated once: a new header
    # would go untracked, and a deleted one leaves ninja demanding a file no
    # rule can produce.
    # The .def files are headers by another name: this image compiles
    # core/str/str.c, which includes core/def/str_sys.def, so editing one has
    # to relink sw.bin. Only this build needs saying -- the RIA and the
    # emulator get it from gcc depfiles.
    file(GLOB_RECURSE SW_HEADERS CONFIGURE_DEPENDS
        ${RP6502_SRC}/core/*.h
        ${RP6502_SRC}/core/def/*.def
        ${RP6502_SRC}/host/pocket/*.h
        ${RP6502_SRC}/host/pico/ria/*.h
        ${RP6502_SRC}/host/pico/vga/*.h)
    set(SW_SOURCES
        ${SW_SRC}/crt0.S ${SW_SRC}/main.c ${SW_SRC}/apf.c ${SW_SRC}/aud.c
        ${SW_SRC}/sst.c
        ${SW_SRC}/cfg.c
        ${RP6502_SRC}/core/com/com.c ${SW_SRC}/com.c ${SW_SRC}/cpu.c ${SW_SRC}/font.c ${SW_SRC}/hid.c
        ${SW_SRC}/layout.c ${SW_SRC}/log.c ${SW_SRC}/mem.c
        ${SW_SRC}/msc.c
        ${SW_SRC}/proc.c ${SW_SRC}/rand.c ${SW_SRC}/rom.c ${SW_SRC}/time.c
        ${SW_SRC}/trap.c ${SW_SRC}/tty.c ${SW_SRC}/uni.c ${SW_SRC}/vga.c ${SW_SRC}/vid.c
        ${RP6502_SRC}/core/aud/bel_presets.c
        ${SW_SRC}/bel.c
        ${RP6502_SRC}/core/sys/pix.c
        ${RP6502_SRC}/core/sys/rom_rec.c
        ${RP6502_SRC}/core/sys/rom_win.c
        ${RP6502_SRC}/core/sys/main_xreg_0.c
        ${RP6502_SRC}/core/sys/main_xreg_1.c
        ${RP6502_SRC}/core/rand.c
        ${RP6502_SRC}/core/main.c
        ${RP6502_SRC}/core/api/api.c
        ${RP6502_SRC}/core/api/arg.c
        ${RP6502_SRC}/core/api/attr.c
        ${RP6502_SRC}/core/api/proc.c
        ${RP6502_SRC}/core/api/clk.c
        ${RP6502_SRC}/core/api/std.c
        ${RP6502_SRC}/core/api/dir.c
        ${RP6502_SRC}/core/api/ops.c
        ${RP6502_SRC}/core/api/uni.c
        # The real HID drivers, told by apf.c what the dock holds. No
        # descriptor ever reaches this machine, so core/hid/parse.c is
        # not here. The layouts are an asset, so layout.c reads them rather
        # than linking twenty kilobytes of table into a 96 KB memory.
        ${RP6502_SRC}/core/hid/vt.c
        ${RP6502_SRC}/core/hid/hid.c
        ${RP6502_SRC}/core/hid/keyboard.c
        ${RP6502_SRC}/core/hid/layout.c
        ${RP6502_SRC}/core/hid/keymap.c
        ${RP6502_SRC}/core/hid/mouse.c
        ${RP6502_SRC}/core/hid/gamepad.c
        ${RP6502_SRC}/core/hid/tablet.c
        ${RP6502_SRC}/core/str/rln.c
        ${RP6502_SRC}/core/str/str.c
        ${RP6502_SRC}/core/vga/canvas.c
        ${RP6502_SRC}/core/vga/mode.c
        ${RP6502_SRC}/core/vga/mode1.c
        ${RP6502_SRC}/core/vga/mode2.c
        ${RP6502_SRC}/core/vga/mode3.c
        ${RP6502_SRC}/core/vga/mode4.c
        ${RP6502_SRC}/core/vga/mode5.c
        ${RP6502_SRC}/core/term/color.c
        ${RP6502_SRC}/core/term/term.c)
    # The firmware's console into a file on the drive, for reading a
    # restore from the inside when the host's debug log has stopped. Off
    # by default: an on build holds one of the drive's eight descriptors
    # for the session, which the conformance suite counts.
    option(RP6502_LOG_FILE "Soft CPU console to /Saves/.../rp6502.log" OFF)
    set(SW_LOG_DEFINE "")
    if(RP6502_LOG_FILE)
        set(SW_LOG_DEFINE -DRP6502_LOG_FILE)
    endif()
    add_custom_command(OUTPUT ${SW_BIN}
        COMMAND ${RISCV_GCC} -march=rv32imac_zicsr_zifencei -mabi=ilp32
            ${SW_LOG_DEFINE}
            # Prologues and epilogues become calls into libgcc's
            # __riscv_save_N/__riscv_restore_N instead of a run of
            # stores. Kilobytes of text for a few cycles a call, and the
            # whole firmware shares one TCM with the stack and the heap,
            # so text is the scarce thing here, not cycles.
            -msave-restore
            -Os -ffreestanding -nostartfiles
            --specs=picolibc.specs -DPICOLIBC_INTEGER_PRINTF_SCANF
            -ffunction-sections -fdata-sections -Wl,--gc-sections -flto
            -I ${RP6502_SRC}/host/pocket
            -I ${RP6502_SRC}
            -I ${RP6502_ASSETS}
            -I ${RP6502_VENDOR}
            "-DPICO_PROGRAM_NAME=\"RP6502-FPGA\""
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
