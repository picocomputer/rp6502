#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The PLL's outputs are in no group. The machine's clocks, clk_a2 and
# clk_ph beside clk_sys, cross synchronously, and one group holding all
# of the PLL's outputs would cut every path between them and clk_74a.
# pocket.sdc cuts or bounds some of those paths by register name, and
# the rest of them are timed.
#

set_clock_groups -asynchronous \
 -group { bridge_spiclk } \
 -group { clk_74a } \
 -group { clk_74b }
