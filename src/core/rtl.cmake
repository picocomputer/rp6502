# The machine, as a list of files.
#
# Whoever builds the machine needs this and none of them needs a simulator to
# have it: the verilated model is built from this list, and so is every Quartus
# project. Guarding it behind verilator_FOUND would mean no bitstream without
# Verilator installed, which CI's bitstream runner does not have.

# Guarded like assets.cmake and gen.cmake. Without it a second include
# re-prepends tests/bench's lint waivers, which rp6502_verilate.cmake puts at
# the front of this list and which are only applied when read first.
include_guard(GLOBAL)

# rp6502_submodule: three of the modules in this list are vendored, and a
# vendored tree is fetched rather than committed.
include(${RP6502_ROOT}/submodules.cmake)
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

set(RP6502_RTL_SOURCES
    ${RP6502_VENDOR}/hazard3_rp6502/hazard3_regfile_1w2r.v
    ${OPL2_SOURCES}
    ${W65C02_ROM}
    ${RP6502_SRC}/core/vga/scan/timing_pkg.sv
    ${RP6502_SRC}/core/riscv/tcm_pkg.sv
    ${RP6502_SRC}/core/wdc/cpu.sv
    ${RP6502_SRC}/core/wdc/bus.sv
    ${RP6502_SRC}/core/wdc/via.sv
    ${RP6502_SRC}/core/wdc/sram.sv
    ${RP6502_SRC}/core/sys/xram.sv
    ${RP6502_SRC}/core/wdc/phi2.sv
    ${RP6502_SRC}/core/ria/regs.sv
    ${RP6502_SRC}/core/riscv/soc.sv
    ${RP6502_SRC}/core/vga/scan/timing.sv
    ${AUD_SINE_PKG}
    ${RP6502_SRC}/core/aud/psg.sv
    ${RP6502_SRC}/core/aud/opl.sv
    ${RSMP_COEF_PKG}
    ${RP6502_SRC}/core/aud/rsmp.sv
    ${VID_PALETTE_PKG}
    ${RP6502_SRC}/core/vga/scan/font.sv
    ${RP6502_SRC}/core/vga/scan/palram.sv
    ${RP6502_SRC}/core/vga/scan/pixtail.sv
    ${RP6502_SRC}/core/vga/scan/sched.sv
    ${RP6502_SRC}/core/vga/scan/fill.sv
    ${RP6502_SRC}/core/vga/scan/linebuf.sv
    ${RP6502_SRC}/core/vga/mode/mode1.sv
    ${RP6502_SRC}/core/vga/mode/mode2.sv
    ${RP6502_SRC}/core/vga/mode/mode3.sv
    ${RP6502_SRC}/core/vga/mode/mode4.sv
    ${RP6502_SRC}/core/vga/mode/mode5.sv
    ${RP6502_SRC}/core/vga/scan/palcache.sv
    ${RP6502_SRC}/core/vga/scan/sbuf.sv
    ${RP6502_SRC}/core/vga/scan/sprite.sv
    ${RP6502_SRC}/core/vga/prog.sv
    ${RP6502_SRC}/core/vga/mode/mode0.sv
    ${RP6502_SRC}/core/vga/scan/compose.sv)
# Verilator elaborates while cmake configures, so an unresolved module here
# is a configure error, not a build one. Nothing recursive: Hazard3 has six
# submodules of its own and this tree reads none of them.
rp6502_submodule(vendor/hazard3 SENTINEL hdl/hazard3_core.v
    WANTS "the soft CPU")
set(RP6502_RTL_VERILATOR_ARGS
    -y ${RP6502_VENDOR}/hazard3/hdl
    -y ${RP6502_VENDOR}/hazard3/hdl/arith
    -y ${RP6502_VENDOR}/hazard3/hdl/debug/dm
    -y ${RP6502_VENDOR}/hazard3/hdl/debug/dtm)
