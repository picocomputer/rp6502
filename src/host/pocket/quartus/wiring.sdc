# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The 6502 and the VIA change state only on the PHI2 enable, except in
# reset and on st_jam, which loads their registers during a savestate
# restore. The firmware limits PHI2 to PHI2_MAX_KHZ, 8000 kHz, which
# against the 50400 kHz clk_sys puts consecutive enables at least six
# clk_sys clocks apart, so a path that one enable launches and the next
# one captures has at least six clocks, and the multicycles on cpu and via
# grant four.

# clk_sys and clk_rv are ports only when wiring is the top level, as it
# is in the machine-only project, where every port is a virtual pin.
# Under core_top the PLL drives both clocks, and all_inputs and
# all_outputs would name the device's pads.
if {[get_collection_size [get_ports -nowarn clk_sys]] > 0} {

    create_clock -name clk_sys -period 19.841 [get_ports clk_sys]
    create_clock -name clk_rv -period 39.682 -waveform {0.000 19.841} \
        [get_ports clk_rv]
    # XRAM's port clock and its phase reference, as pocket_pll.v shifts them.
    create_clock -name clk_a2 -period 9.921 -waveform {8.990 13.951} \
        [get_ports clk_a2]
    create_clock -name clk_ph -period 19.841 -waveform {12.400 22.321} \
        [get_ports clk_ph]

    set_false_path -from [all_inputs] -to [all_registers]
    set_false_path -from [all_registers] -to [all_outputs]
}

derive_clock_uncertainty

# XRAM's port A data registers are on clk_a2 but take a word only on the
# first of its two edges after the machine's, the one xram.sv's ph gate
# names, so what the machine's registers read was launched 12.4 ns before
# their edge and not 2.5. The hold check stays on the edges the data
# really moves on.
set_multicycle_path -setup -start -from [get_registers \
    {*xram:xram|xram_f_rdata[*] *xram:xram|xram_s_rdata[*]}] 2
set_multicycle_path -hold  -start -from [get_registers \
    {*xram:xram|xram_f_rdata[*] *xram:xram|xram_s_rdata[*]}] 1

set_multicycle_path -setup -from [get_registers {*cpu:*}] 4
set_multicycle_path -hold  -from [get_registers {*cpu:*}] 3
set_multicycle_path -setup -to [get_registers {*cpu:*}] 4
set_multicycle_path -hold  -to [get_registers {*cpu:*}] 3

set_multicycle_path -setup -from [get_registers {*via:*}] 4
set_multicycle_path -hold  -from [get_registers {*via:*}] 3
set_multicycle_path -setup -to [get_registers {*via:*}] 4
set_multicycle_path -hold  -to [get_registers {*via:*}] 3

# When a PHI2 enable launches a port A access, pocket_sram captures the
# 6502's byte on the fourth clock after that enable, and the 6502 reads it
# on the next enable, at least six clocks after that launch, so the path
# from that capture into the 6502 has two clocks. This rule names both
# ends, so it takes precedence over the one-ended rule above.
set_multicycle_path -setup -from [get_registers {*pocket_sram*}] \
    -to [get_registers {*cpu:*}] 2
set_multicycle_path -hold  -from [get_registers {*pocket_sram*}] \
    -to [get_registers {*cpu:*}] 1

set_multicycle_path -setup -from [get_registers {*regs*}] 4
set_multicycle_path -hold  -from [get_registers {*regs*}] 3

set_multicycle_path -setup -to [get_registers {*pocket_sram*pocket_sram_a[*]}] 4
set_multicycle_path -hold  -to [get_registers {*pocket_sram*pocket_sram_a[*]}] 3
set_multicycle_path -setup -to [get_registers {*pocket_sram*pocket_sram_dq_out[*]}] 4
set_multicycle_path -hold  -to [get_registers {*pocket_sram*pocket_sram_dq_out[*]}] 3

# dram_clk is clk_dram forwarded through a DDR output cell, and clk_dram
# runs half a clk_sys period behind clk_sys. The SDRAM samples its inputs
# on dram_clk as it arrives at the chip, so the generated clock is
# declared at the pad.
if {[get_collection_size [get_ports -nowarn {dram_clk}]] > 0} {
    create_generated_clock -name dram_clk_pin \
        -source [get_pins {ic|pll|pll|general[2].gpll~PLL_OUTPUT_COUNTER|divclk}] \
        [get_ports {dram_clk}]

    set dram_out [get_ports {dram_a[*] dram_ba[*] dram_dq[*] dram_dqm[*] \
        dram_cke dram_ras_n dram_cas_n dram_we_n}]
    set_output_delay -clock dram_clk_pin -max  2.0 $dram_out
    set_output_delay -clock dram_clk_pin -min -1.0 $dram_out
    set_input_delay  -clock dram_clk_pin -max  6.0 [get_ports {dram_dq[*]}]
    set_input_delay  -clock dram_clk_pin -min  2.5 [get_ports {dram_dq[*]}]

    # S_READ sets rd_pipe[0], and pocket_sdram_rdata loads only when
    # rd_pipe[3] is set, on the fourth clk_sys edge after the one that
    # issues the READ. rd_pipe is an enable, so without this rule the
    # analyzer times the data against the first clk_sys edge after the
    # dram_clk_pin edge that launches it, 9.92 ns after launch, which
    # leaves 3.92 ns after the 6.0 ns input delay. A multicycle of two
    # leaves 23.76 ns.
    set_multicycle_path -setup -from [get_ports {dram_dq[*]}] \
        -to [get_registers {*pocket_sdram*pocket_sdram_rdata*}] 2
    set_multicycle_path -hold  -from [get_ports {dram_dq[*]}] \
        -to [get_registers {*pocket_sdram*pocket_sdram_rdata*}] 1
}

# pocket_sram loads pocket_sram_a and pocket_sram_dq_out for the soft
# CPU on the first clk_sys edge after soc_bus_pend rises when pocket_sram
# is idle, so the path from soc into them is one clock. These rules name
# both ends, so they take precedence over the one-ended multicycles of
# four above.
set_multicycle_path -setup -from [get_registers {*soc*}] \
    -to [get_registers {*pocket_sram*pocket_sram_a[*]}] 1
set_multicycle_path -hold  -from [get_registers {*soc*}] \
    -to [get_registers {*pocket_sram*pocket_sram_a[*]}] 0
set_multicycle_path -setup -from [get_registers {*soc*}] \
    -to [get_registers {*pocket_sram*pocket_sram_dq_out[*]}] 1
set_multicycle_path -hold  -from [get_registers {*soc*}] \
    -to [get_registers {*pocket_sram*pocket_sram_dq_out[*]}] 0

# The board's SRAM, an AS6C2016-55BIN, has no clock, so there are no I/O
# delays to declare, and the interface is bounded by its round trip:
#
#   4 clk_sys                     79.365 ns
#   tAA, address to data valid   -55.000 ns
#                                 ------
#   left for both pad crossings    24.365 ns
#
# Eleven nanoseconds each way spends 22 ns of that and leaves 2.365 ns for
# clock uncertainty and the board.
if {[get_collection_size [get_ports -nowarn {sram_a[0]}]] > 0} {
    set sram_pads [get_ports {sram_a[*] sram_dq[*] sram_oe_n sram_we_n \
        sram_ub_n sram_lb_n}]
    set_max_delay -to $sram_pads 11.0
    set_min_delay -to $sram_pads 0.0
    set_max_delay -from [get_ports {sram_dq[*]}] 11.0
    set_min_delay -from [get_ports {sram_dq[*]}] 0.0
}
