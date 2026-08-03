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

set_multicycle_path -setup -from [get_registers {*cpu65*}] 4
set_multicycle_path -hold  -from [get_registers {*cpu65*}] 3
set_multicycle_path -setup -to [get_registers {*cpu65*}] 4
set_multicycle_path -hold  -to [get_registers {*cpu65*}] 3

set_multicycle_path -setup -from [get_registers {*via*}] 4
set_multicycle_path -hold  -from [get_registers {*via*}] 3
set_multicycle_path -setup -to [get_registers {*via*}] 4
set_multicycle_path -hold  -to [get_registers {*via*}] 3
