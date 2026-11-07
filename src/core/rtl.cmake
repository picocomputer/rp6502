include_guard(GLOBAL)

include(${RP6502_ROOT}/submodules.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/assets.cmake)

set(RP6502_RTL_SOURCES
    ${RP6502_VENDOR}/hazard3_rp6502/hazard3_regfile_1w2r.v
    ${W65C02_ROM}
    ${RP6502_SRC}/core/vga/scan/timing_pkg.sv
    ${RP6502_SRC}/core/vga/canvas_pkg.sv
    ${RP6502_SRC}/core/vga/mode/mode.sv
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
    ${OPL_ROM_PKG}
    ${RP6502_SRC}/core/aud/opl.sv
    ${RSMP_COEF_PKG}
    ${RP6502_SRC}/core/aud/pcm.sv
    ${VID_PALETTE_PKG}
    ${RP6502_SRC}/core/vga/scan/font.sv
    ${RP6502_SRC}/core/vga/scan/palram.sv
    ${RP6502_SRC}/core/vga/scan/pixtail.sv
    ${RP6502_SRC}/core/vga/scan/rowmap.sv
    ${RP6502_SRC}/core/vga/scan/sched.sv
    ${RP6502_SRC}/core/vga/scan/fill.sv
    ${RP6502_SRC}/core/vga/scan/linebuf.sv
    ${RP6502_SRC}/core/vga/mode/mode1.sv
    ${RP6502_SRC}/core/vga/mode/mode2.sv
    ${RP6502_SRC}/core/vga/mode/mode3.sv
    ${RP6502_SRC}/core/vga/mode/mode4.sv
    ${RP6502_SRC}/core/vga/mode/mode5.sv
    ${RP6502_SRC}/core/vga/scan/palcache.sv
    ${RP6502_SRC}/core/vga/scan/rowq.sv
    ${RP6502_SRC}/core/vga/scan/listq.sv
    ${RP6502_SRC}/core/vga/scan/sbuf.sv
    ${RP6502_SRC}/core/vga/scan/sprite.sv
    ${RP6502_SRC}/core/vga/prog.sv
    ${RP6502_SRC}/core/vga/mode/mode0.sv
    ${RP6502_SRC}/core/vga/scan/compose.sv)
rp6502_submodule(vendor/hazard3 SENTINEL hdl/hazard3_core.v
    WANTS "the soft CPU")
set(RP6502_RTL_VERILATOR_ARGS
    -y ${RP6502_VENDOR}/hazard3/hdl
    -y ${RP6502_VENDOR}/hazard3/hdl/arith
    -y ${RP6502_VENDOR}/hazard3/hdl/debug/dm
    -y ${RP6502_VENDOR}/hazard3/hdl/debug/dtm)
