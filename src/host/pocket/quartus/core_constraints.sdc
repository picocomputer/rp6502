#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The PLL's outputs are in no group. clk_sys and clk_rv come from one
# PLL, clk_rv at half the rate of clk_sys and rising with it, and the
# soft CPU's interface crosses between them synchronously on about a
# million and a quarter timed paths, so putting the two in separate
# asynchronous groups would cut every one of those paths from analysis.
# One group holding all of the PLL's outputs would instead cut every
# path between those outputs and clk_74a. pocket.sdc cuts or bounds some
# of those paths by register name, and the rest of them are timed.
#

set_clock_groups -asynchronous \
 -group { bridge_spiclk } \
 -group { clk_74a } \
 -group { clk_74b }
