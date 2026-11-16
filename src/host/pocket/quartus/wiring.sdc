# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The 6502 and the VIA change state only on the PHI2 enable, except in
# reset and on st_jam, which loads their registers during a savestate
# restore. wiring.sv holds st_jam for four clocks with the enable held
# low, and loads the same words on each. The firmware limits PHI2 to
# PHI2_MAX_KHZ, 8000 kHz, which
# against the 50400 kHz clk_sys puts consecutive enables at least six
# clk_sys clocks apart, so a path that one enable launches and the next
# one captures has at least six clocks, and the multicycles on cpu and via
# grant four.

# The clocks are ports only when wiring is the top level, as it is in
# the machine-only project, where every port is a virtual pin. Under
# core_top the PLL drives them, and all_inputs and all_outputs would
# name the device's pads.
if {[get_collection_size [get_ports -nowarn clk_sys]] > 0} {

    create_clock -name clk_sys -period 19.841 [get_ports clk_sys]
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

set_multicycle_path -setup -from [get_registers {*regs:*}] 4
set_multicycle_path -hold  -from [get_registers {*regs:*}] 3

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

    # pocket_sdram's dq_cap takes dram_dq on every falling edge of
    # clk_sys, and nothing else is fed by the pins. The falling edge of
    # clk_sys is the rising edge of dram_clk, so a word that one
    # dram_clk_pin edge launches is latched on the next, a full period
    # later: 19.84 ns, less the 6.0 ns input delay and the time dram_clk
    # takes to reach its pad. The PLL's shift is 9920 ps where half a
    # period is 9920.6, so on paper a falling edge of clk_sys comes
    # 1 ps after the launch and the analyzer would time the capture
    # against that one. The setup multicycle of two names the edge a
    # period later. No hold multicycle goes with it: the hold check is
    # then the edge before the capture, which is the 1 ps edge, so the
    # word must not change until 1 ps after its own launch. The chip
    # holds it 2.5 ns, the minimum input delay.
    set_multicycle_path -setup -from [get_ports {dram_dq[*]}] \
        -to [get_registers {*pocket_sdram*dq_cap*}] 2
}

