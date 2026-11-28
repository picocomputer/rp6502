# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The 6502 and the VIA change state only on the PHI2 enable, except in
# reset and on st_jam, which loads their registers during a savestate
# restore. wiring.sv holds st_jam for four clocks with the enable held
# low, and loads the same words on each. The firmware limits PHI2 to
# PHI2_MAX_KHZ, 8000 kHz, which against the 50400 kHz clk_sys puts
# consecutive enables at least six clk_sys clocks apart, so a path that one
# enable launches and the next one captures has at least six clocks, and
# the multicycles on cpu and via grant four.
#
# The host's SDC creates the clocks and derives their uncertainty.

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
