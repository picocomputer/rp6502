include_guard(GLOBAL)

include(${RP6502_ROOT}/submodules.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/gen.cmake)
rp6502_gen_assets()

set(RP6502_RTL_SOURCES
    ${RP6502_VENDOR}/hazard3_rp6502/hazard3_core.v
    ${RP6502_VENDOR}/hazard3_rp6502/hazard3_decode.v
    ${RP6502_VENDOR}/hazard3_rp6502/hazard3_frontend.v
    ${RP6502_VENDOR}/hazard3_rp6502/hazard3_instr_decompress.v
    ${RP6502_VENDOR}/hazard3_rp6502/hazard3_regfile_1w2r.v
    ${RP6502_ASSETS}/cpu_rom_pkg.sv
    ${RP6502_SRC}/core/vga/scan/timing_pkg.sv
    ${RP6502_SRC}/core/vga/canvas_pkg.sv
    ${RP6502_SRC}/core/vga/mode/mode.sv
    ${RP6502_SRC}/core/riscv/mem_pkg.sv
    ${RP6502_SRC}/core/wdc/cpu.sv
    ${RP6502_SRC}/core/wdc/bus.sv
    ${RP6502_SRC}/core/wdc/via.sv
    ${RP6502_SRC}/core/wdc/sram.sv
    ${RP6502_SRC}/core/sys/xram.sv
    ${RP6502_SRC}/core/wdc/phi2.sv
    ${RP6502_SRC}/core/ria/regs.sv
    ${RP6502_SRC}/core/riscv/cache.sv
    ${RP6502_SRC}/core/riscv/soc.sv
    ${RP6502_SRC}/core/vga/scan/timing.sv
    ${RP6502_ASSETS}/psg_pkg.sv
    ${RP6502_SRC}/core/aud/psg.sv
    ${RP6502_ASSETS}/opl_rom_pkg.sv
    ${RP6502_SRC}/core/aud/opl.sv
    ${RP6502_ASSETS}/rsmp_coef_pkg.sv
    ${RP6502_SRC}/core/aud/pcm.sv
    ${RP6502_ASSETS}/color_pkg.sv
    ${RP6502_ASSETS}/mode6_pkg.sv
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
    ${RP6502_SRC}/core/vga/mode/mode6.sv
    ${RP6502_SRC}/core/vga/scan/sprite.sv
    ${RP6502_SRC}/core/vga/prog.sv
    ${RP6502_SRC}/core/vga/mode/mode0.sv
    ${RP6502_SRC}/core/vga/scan/compose.sv
    ${RP6502_SRC}/core/sys/wiring.sv)
set(RP6502_RTL_SDC ${RP6502_SRC}/core/sys/wiring.sdc)
rp6502_submodule(vendor/hazard3 SENTINEL hdl/hazard3_core.v
    WANTS "the soft CPU")
# The rest of the soft CPU: the hazard3 modules that RP6502_RTL_SOURCES does
# not override, and the headers they and the overrides include. A Quartus
# project can list these instead of searching hazard3's folders, so that
# every file it reads is one it was given.
set(RP6502_RTL_HAZARD3
    ${RP6502_VENDOR}/hazard3/hdl/hazard3_cpu_1port.v
    ${RP6502_VENDOR}/hazard3/hdl/hazard3_csr.v
    ${RP6502_VENDOR}/hazard3/hdl/hazard3_power_ctrl.v
    ${RP6502_VENDOR}/hazard3/hdl/arith/hazard3_alu.v
    ${RP6502_VENDOR}/hazard3/hdl/arith/hazard3_shift_barrel.v
    ${RP6502_VENDOR}/hazard3/hdl/arith/hazard3_priority_encode.v
    ${RP6502_VENDOR}/hazard3/hdl/arith/hazard3_onehot_priority.v
    ${RP6502_VENDOR}/hazard3/hdl/arith/hazard3_onehot_encode.v
    ${RP6502_VENDOR}/hazard3/hdl/arith/hazard3_muldiv_seq.v
    ${RP6502_VENDOR}/hazard3/hdl/arith/hazard3_branchcmp.v)
set(RP6502_RTL_INCLUDES
    ${RP6502_VENDOR}/hazard3/hdl/hazard3_config.vh
    ${RP6502_VENDOR}/hazard3/hdl/hazard3_config_inst.vh
    ${RP6502_VENDOR}/hazard3/hdl/hazard3_csr_addr.vh
    ${RP6502_VENDOR}/hazard3/hdl/hazard3_ops.vh
    ${RP6502_VENDOR}/hazard3/hdl/hazard3_width_const.vh
    ${RP6502_VENDOR}/hazard3/hdl/rv_opcodes.vh)
set(RP6502_RTL_VERILATOR_ARGS
    -y ${RP6502_VENDOR}/hazard3/hdl
    -y ${RP6502_VENDOR}/hazard3/hdl/arith
    -y ${RP6502_VENDOR}/hazard3/hdl/debug/dm
    -y ${RP6502_VENDOR}/hazard3/hdl/debug/dtm)
