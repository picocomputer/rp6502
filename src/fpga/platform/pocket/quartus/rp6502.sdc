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

create_clock -name clk_sys -period 19.841 [get_ports clk_sys]
derive_clock_uncertainty

# The machine's ports are testbench-facing; the platform owns the pads.
set_false_path -from [all_inputs] -to [all_registers]
set_false_path -from [all_registers] -to [all_outputs]

set_multicycle_path -setup -from [get_registers {*cpu65*}] 4
set_multicycle_path -hold  -from [get_registers {*cpu65*}] 3
set_multicycle_path -setup -to [get_registers {*cpu65*}] 4
set_multicycle_path -hold  -to [get_registers {*cpu65*}] 3

set_multicycle_path -setup -from [get_registers {*via*}] 4
set_multicycle_path -hold  -from [get_registers {*via*}] 3
set_multicycle_path -setup -to [get_registers {*via*}] 4
set_multicycle_path -hold  -to [get_registers {*via*}] 3
