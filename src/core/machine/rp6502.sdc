# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The machine's timing, as the Pocket's PLL will present it. This
# constrains the machine alone; the platform's own clocks and pads are
# constrained beside core_top when that arrives.
#
# Only two things here are claims about the design rather than about the
# clock, and both are read off the RTL: the 6502 and the VIA advance on
# the PHI2 enable, which is 504 system clocks at the slowest PHI2 the
# machine allows and six at the fastest, that being 50400 kHz over the
# 8000 the accumulator is asked for. Six is the number that matters and
# four is the conservative telling of it. Nothing else has an enable this
# file is willing
# to swear to — an enable that fires on consecutive clocks in some state
# is a single-cycle path however slow its cadence looks from outside,
# and a multicycle written on the strength of a sample rate would be a
# constraint that lies.

# Only when the machine is the top level. Inside a platform its clocks
# arrive from a PLL and its ports are not ports, so every line below
# named one and matched nothing: Quartus printed "Ignored create_clock,
# argument <targets> is an empty collection" for both, in every Pocket
# fit this tree has ever run. The two lines that follow them were worse
# than dead — under a platform, all_inputs and all_outputs are the
# device's pads rather than the machine's testbench edges, so a rule
# written to excuse a bench was cutting real pins. Guard the lot on the
# one condition that separates the two cases.
if {[get_collection_size [get_ports -nowarn clk_sys]] > 0} {

    create_clock -name clk_sys -period 19.841 [get_ports clk_sys]
    # The soft CPU's, half clk_sys and rising with it. Declared against
    # the same waveform rather than as an independent clock, because
    # that is what the PLL makes and what the crossings are written
    # against: the analyzer must see the two as one synchronous group,
    # not as a domain crossing it should cut. Under the platform the
    # same statement is made by core_constraints.sdc, which names the
    # PLL's own outputs and puts them in one group.
    create_clock -name clk_rv -period 39.682 -waveform {0.000 19.841} \
        [get_ports clk_rv]

    # The machine's ports are testbench-facing; the platform owns the pads.
    set_false_path -from [all_inputs] -to [all_registers]
    set_false_path -from [all_registers] -to [all_outputs]
}

derive_clock_uncertainty

set_multicycle_path -setup -from [get_registers {*cpu:*}] 4
set_multicycle_path -hold  -from [get_registers {*cpu:*}] 3
set_multicycle_path -setup -to [get_registers {*cpu:*}] 4
set_multicycle_path -hold  -to [get_registers {*cpu:*}] 3

set_multicycle_path -setup -from [get_registers {*via:*}] 4
set_multicycle_path -hold  -from [get_registers {*via:*}] 3
set_multicycle_path -setup -to [get_registers {*via:*}] 4
set_multicycle_path -hold  -to [get_registers {*via:*}] 3

# The rules above are one-ended, and the path arriving at the 6502 from
# the SRAM does not get six clocks to settle. Left to the -to rule above
# it would be analysed against 79.4 ns, close on paper, and let the
# fitter place those registers wherever it liked.
#
# Two, counted off the RTL: the launch is on the enable edge, the chip
# takes four clocks, the byte is captured on the fourth, and the 6502
# samples it on the sixth. Two-ended beats one-ended in SDC precedence.
#
# This said 1 for a while, from when the launch was a clock later and
# the capture landed on the fifth. Moving the launch earlier to buy the
# address cone its second clock is exactly what makes 2 the right
# number, and the constraint did not follow the design.
set_multicycle_path -setup -from [get_registers {*pocket_sram*}] \
    -to [get_registers {*cpu:*}] 2
set_multicycle_path -hold  -from [get_registers {*pocket_sram*}] \
    -to [get_registers {*cpu:*}] 1

# regs advances on phi2_en exactly as w65c02 and via do, and it
# feeds the 6502's data bus. Left unnamed, the path from it through the
# address cone into the SRAM's launch register was analysed against one
# clock and missed by 4.3 ns — for a path whose both ends move six
# clocks apart.
set_multicycle_path -setup -from [get_registers {*regs*}] 4
set_multicycle_path -hold  -from [get_registers {*regs*}] 3

# The SRAM's launch registers are the other end of that cycle. This grant
# is the 6502's alone; port B shares the registers and is walked back to
# one clock below, because a shared register otherwise takes the loosest
# rule written against it. The capture register is deliberately NOT in
# here: its path into w65c02 has one clock and the rule above it already
# says so.
set_multicycle_path -setup -to [get_registers {*pocket_sram*pocket_sram_a[*]}] 4
set_multicycle_path -hold  -to [get_registers {*pocket_sram*pocket_sram_a[*]}] 3
set_multicycle_path -setup -to [get_registers {*pocket_sram*pocket_sram_dq_out[*]}] 4
set_multicycle_path -hold  -to [get_registers {*pocket_sram*pocket_sram_dq_out[*]}] 3

# The SDRAM, AS4C32M16MSA-6BIN. These pins carried no constraint at all
# until now, which mattered more here than anywhere else on the board:
# CKE arriving one edge late turns a self refresh entry into an ordinary
# auto refresh followed by a precharge power-down, and a chip in
# power-down does not refresh itself. That failure looks exactly like
# the one that broke asset reads today, it is invisible to the bench —
# sdram_model treats CKE low as self refresh by construction — and it
# would arrive or not arrive with the fitter's mood.
#
# dram_clk is clk_dram forwarded through a DDR output cell, so the clock
# the chip samples on is declared where it leaves: at the pad. The half
# period of shift is what gives the command and address outputs their
# time, and it only counts if the analyzer knows about it.
#
# The numbers are the chip's own input requirements, all four groups
# alike: address tCAS/tCAH, control tCMS/tCMH, data tCDS/tCDH and CKE
# tCKS/tCKH are 2.0 ns of setup and 1.0 ns of hold. Read data comes back
# tAC 6.0 ns after the edge at CL2 and holds tOH 2.5 ns. The board adds
# almost nothing — the part sits as close to the die as it can be
# placed.
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

    # The read return takes a wait state, and a wait state is invisible
    # to the analyzer unless it is declared. rd_pipe is an ENABLE on the
    # capture register, so STA sees a flop clocked every clk_sys and
    # analyses the launch against the very next edge — 9.92 ns, of which
    # tAC takes 6, leaving 3.92 for a pad crossing that is about seven.
    # Adding the wait state in RTL moved that number by fourteen
    # picoseconds, because the RTL was never what STA was reading.
    #
    # Two, counted off the FSM: S_READ sets rd_pipe[0] and the capture is
    # gated on rd_pipe[3], one clk_sys later than it used to be. That
    # turns 3.92 ns into 23.76.
    set_multicycle_path -setup -from [get_ports {dram_dq[*]}] \
        -to [get_registers {*pocket_sdram*pocket_sdram_rdata*}] 2
    set_multicycle_path -hold  -from [get_ports {dram_dq[*]}] \
        -to [get_registers {*pocket_sdram*pocket_sdram_rdata*}] 1
}

# The soft CPU's own path into that launch register is ONE clk_sys edge,
# not four. The rule above is one-ended and sweeps it up with the 6502's,
# which is a lie in the direction that corrupts memory: WE# goes low at
# phase zero, so an address still settling means the write pulse is live
# across every address the bus sweeps through on its way. A program
# loaded through this port then has bytes scattered at addresses nobody
# chose, the image CRC passes because the CRC is taken on the way out of
# the SDRAM, and the 6502 runs into whatever landed.
#
# That is the intermittent one-in-five load failure: staged fine, parsed
# fine, and then nothing. It was hidden for a while behind an eight-clock
# port B access, written on the reasoning that a late address still
# leaves enough stable time for tAA — true of a read and worthless for a
# write, which is why the port is four clocks again now that these two
# rules say the real thing.
set_multicycle_path -setup -from [get_registers {*soc*}] \
    -to [get_registers {*pocket_sram*pocket_sram_a[*]}] 1
set_multicycle_path -hold  -from [get_registers {*soc*}] \
    -to [get_registers {*pocket_sram*pocket_sram_a[*]}] 0
set_multicycle_path -setup -from [get_registers {*soc*}] \
    -to [get_registers {*pocket_sram*pocket_sram_dq_out[*]}] 1
set_multicycle_path -hold  -from [get_registers {*soc*}] \
    -to [get_registers {*pocket_sram*pocket_sram_dq_out[*]}] 0

# The board's SRAM, AS6C2016-55BIN. It has no clock, so there is no
# input or output delay to declare against one — what bounds this
# interface is the round trip, and the round trip has a budget:
#
#   4 clk_sys                     79.365 ns
#   tAA, address to data valid   -55.000 ns
#                                 ------
#   left for both pad crossings    24.365 ns
#
# Eleven each way spends 22 of that and leaves 2.365 for clock
# uncertainty and the board, which is almost nothing here — the part
# sits as close to the die as it can be placed. If the fitter cannot
# meet these it will say so, which is the entire point: these pins were
# among the 148 carrying no constraint at all, so every timing number
# anyone quoted about this interface was arithmetic rather than
# analysis.
#
# Guarded by -nowarn because the machine-only projects have no pads.
if {[get_collection_size [get_ports -nowarn {sram_a[0]}]] > 0} {
    set sram_pads [get_ports {sram_a[*] sram_dq[*] sram_oe_n sram_we_n \
        sram_ub_n sram_lb_n}]
    set_max_delay -to $sram_pads 11.0
    set_min_delay -to $sram_pads 0.0
    set_max_delay -from [get_ports {sram_dq[*]}] 11.0
    set_min_delay -from [get_ports {sram_dq[*]}] 0.0
}
