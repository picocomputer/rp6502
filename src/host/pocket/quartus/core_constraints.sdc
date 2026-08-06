#
# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The core's clock groups. apf_constraints.sdc reads
# core/core_constraints.sdc by that path, so this stands in for the copy
# in vendor/openfpga, which groups a PLL this core does not instantiate.
#
# The template names four outputs under
# ic|mp1|mf_pllbase_inst|altera_pll_i, which is what its own reference
# core calls them. Ours is a pocket_pll called pll inside core_top, so
# all four filters matched nothing and Quartus said so eight times in
# every fit — four "could not be matched with a clock", four "accepted
# but has some problems". They went unread for months.
#
# The three host clocks below are what the template got right, and they
# are kept. The four PLL outputs are deliberately left out, which is not
# the same as leaving the mistake in place:
#
# The template puts each output in a group of its own, declaring them
# mutually asynchronous. That is false here. clk_sys and clk_rv are one
# PLL, an integer ratio apart, rising together, and the soft CPU's
# entire interface crosses between them synchronously — a million and a
# quarter timed paths. Naming them correctly in separate groups would
# cut every one of those and stop the machine being analysed at all,
# which is a worse outcome than the warning.
#
# Putting all four in one group instead would be true, and still costs
# something: it would cut them from clk_74a, and those crossings are
# timed today and close. The queues and synchronisers between the two
# would then rest on pocket.sdc's bounds alone. Analysis that works is
# worth more than a tidier file, so they stay ungrouped and the
# relationship stays measured.
#
# What the crossings actually need is in pocket.sdc: the first stage of
# every synchroniser is cut by destination, and the payloads that ride
# beside a crossing pointer are bounded rather than cut.
#

set_clock_groups -asynchronous \
 -group { bridge_spiclk } \
 -group { clk_74a } \
 -group { clk_74b }
