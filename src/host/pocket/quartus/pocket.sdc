# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The platform's own constraints: what the machine's SDC cannot know,
# because it does not know there is a second clock.
#
# pocket_fifo joins two domains — the host's 74.25 MHz to the machine's,
# and the machine's back to the host's — with gray-coded pointers and
# two-flop synchronizers. The analyzer has no way to see that from the
# netlist and times the first flop of each synchronizer as if the launch
# were related to the capture, which produces a large negative number
# about a path that is asynchronous by construction.
#
# Cutting it is safe here for the reason gray coding exists: only one
# bit of the pointer changes per step, so a sample taken while it is
# changing reads either the value before or the value after, and both
# are pointers the reader may legitimately act on. Skew between the bits
# cannot manufacture a third value.
#
# The synchronizer's own second stage stays timed — it is an ordinary
# path inside the destination domain, and metastability settling is
# what the two flops are for.

# Cut by destination, not by source: the pointer is converted to gray
# combinationally on its way out, so the register that launches is the
# binary one and naming the gray register misses half the paths. Every
# path arriving at a synchronizer's first stage is asynchronous whatever
# it came from, which is the whole reason the stage is there.

set_false_path -to [get_registers {*pocket_fifo*|wptr_gray_r1[*]}]
set_false_path -to [get_registers {*pocket_fifo*|rptr_gray_w1[*]}]

# The bridge's write payload rides beside its own gray pointer and is
# only read once that pointer has crossed, so it is quasi-static by the
# time the reader looks. It carried a bound of one host clock here for a
# long time, and the bound never once applied.
#
# Twice, in fact. It named {*pocket_fifo*|mem*}, and mem is an inferred
# MLAB: not a register, so get_registers cannot see it, and the fitter
# said "Ignored filter" in every fit this tree has run. Named correctly
# as the memory's ~MEMORYREGOUT it matches 136 keepers at signoff and
# still not during placement, because those atoms do not exist until the
# fitter makes them — so a bound meant to steer the placer could only
# ever grade it afterwards.
#
# It is gone rather than fixed a third time, because it was bracing
# something that does not need it. core_constraints.sdc groups only the
# host's clocks; the PLL's outputs are in no group, so nothing cuts
# these crossings and the analyzer times them the ordinary way, more
# tightly than one host clock. A constraint that adds nothing but a
# warning is worse than no constraint.
#
# If the PLL outputs are ever put in a group of their own, this becomes
# load-bearing again — and it must then be written by clock pair rather
# than by node, so that it resolves at every stage instead of only at
# the last one.

# The rest of the crossings between the host's clock and the machine's.
# Every one is a two-flop synchronizer or a value held still behind one,
# and the analyzer times the first stage as though the launch were
# related to the capture. Cut by destination for the same reason as the
# queues above: what launches does not matter, arriving at a first stage
# is what makes a path asynchronous.

# synch_3 is Analogue's synchronizer and ours by instantiation.
set_false_path -to [get_registers {*synch_3*|stage_1[*]}]

# The platform's own first stages. Named by convention — _s1 for a level
# crossing, _t1 for a toggle — so the rule is the convention rather than
# a list that goes stale the next time one is added: reset_n_s1, keys_s1,
# settle_t1, urst_t1, frame_t1 today.
set_false_path -to [get_registers {*|*_s1}]
set_false_path -to [get_registers {*|*_s1[*]}]
set_false_path -to [get_registers {*|*_t1}]

# The staged image's length crosses as data beside a toggle, and is only
# read once that toggle has settled — quasi-static by the time anything
# looks at it. Bounded rather than cut: it must not take longer to cross
# than the toggle that announces it.
set_max_delay -from [get_registers {*pocket_bridge*|slot_size[*]}] \
    -to [get_registers {*pocket_bridge*|pocket_bridge_slot_len[*]}] 13.468
set_min_delay -from [get_registers {*pocket_bridge*|slot_size[*]}] \
    -to [get_registers {*pocket_bridge*|pocket_bridge_slot_len[*]}] 0

# The same length, crossing on into the soft CPU. This leg is
# synchronous — 50.4 into 25.2, rising together — but the bus still
# stands still when it is sampled: the bridge writes it on the settle
# toggle and fires slot_set four machine clocks later, and rv_soc
# captures only under that enable. The min-delay-0 idiom does nothing
# here because the same-edge hold relationship is already zero; what is
# void is the hold check itself, which guards a same-edge change the
# announcement's four-clock lead excludes. Setup stays, and matters:
# the bus must still cross before the enable does.
set_false_path -hold \
    -from [get_registers {*pocket_bridge*|pocket_bridge_slot_len[*]}] \
    -to [get_registers {*|mmio_slot_len[*]}]

# The machine's two spines. clk_sys is the PLL's general[0]; clk_rv and
# the beam clock share general[1]; every transfer between them is
# same-edge synchronous with a hold relationship of zero, met only by
# the skew between two global networks staying under one LUT of data
# delay. The fitter pads such paths against its own delay estimate, and
# two fits in a row it left one fast-corner path a handful of
# picoseconds short — a different register pair each time, because
# which pair draws the worst spine seam is placement lottery. What is
# actually untrusted is the corner model on this crossing, by tens of
# picoseconds, so say exactly that: sixty picoseconds of added hold
# uncertainty, three times the worst observed miss, in both directions.
#
# Raising it does not scale. A later fit missed by 124 ps on rv_soc's
# dph_addr into the staging read port; 190 ps of uncertainty moved that
# to 73 and took setup from 1.129 to 0.906, because the demand rises
# faster than the fitter can pay it. That path wanted an exception, not
# a bigger number — see the staging capture below.
# The fitter then pads every such path to clear it and the signoff
# demands the same. A flat minimum delay was tried instead and asked
# too much of paths into MLAB address ports, which have almost no
# routing detour to give; uncertainty scales the demand to the check.
set_clock_uncertainty -add -hold 0.060 \
    -from [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}] \
    -to [get_clocks {*|general[1].gpll~PLL_OUTPUT_COUNTER|divclk}]
set_clock_uncertainty -add -hold 0.060 \
    -from [get_clocks {*|general[1].gpll~PLL_OUTPUT_COUNTER|divclk}] \
    -to [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}]

# The same distrust, within one network. With the crossing padded,
# every fit floors its fast-corner hold in the same two places: the
# sprite gatherer's registers into d_t on the machine clock, and the
# vendor SPI receiver on its own — ordinary same-clock transfers whose
# launch and capture drew distant leaves of one global tree. Real
# paths, so no exception can apply. A CI miss was once pinned on this
# family from these local floors, wrongly — the paths report later
# named that miss as the crossing's — but the floors are thin all the
# same and the refit priced the pad at nothing: eighty picoseconds,
# three times the misses this seam class produces, on the two clocks
# that floor thin, and nothing on clk_74a, whose quietest path clears
# three times this demand already.
set_clock_uncertainty -add -hold 0.080 \
    -from [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}] \
    -to [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}]
set_clock_uncertainty -add -hold 0.080 \
    -from [get_clocks {bridge_spiclk}] \
    -to [get_clocks {bridge_spiclk}]

# The data-phase payload, cut at the protocol rather than an endpoint
# at a time. In rv_soc, dph_addr and dph_strb are written under
# "if (hready)" and nowhere else; hready = !(dph_active && dph_ext &&
# !dph_waited) and bus_pend is that same expression un-negated, off the
# same three registers — so hready is exactly !pend. Every machine-side
# capture of the payload is gated on pend, as an enable, a write
# strobe, or an AND that masks the payload when the strobe is low. The
# payload cannot launch on an edge that captures it. Not a timing
# separation, an interlock: it holds however the two clocks skew and
# whatever the fit does with them.
#
# Worth saying because the obvious argument is wrong. "The strobe is a
# clk_sys edge later than the address" is false — a staging read that
# stalls and unstalls mid-period puts bus_stb on an edge that is also a
# clk_rv edge. Three independent attempts to break the interlock instead
# found nothing, one of them reading the fitted netlist to confirm both
# negedge flops and the enable's fanin survived.
#
# This began as one endpoint and the fit lottery walked the family a
# pair at a time: dph_addr into stage_addr_q, then into pocket_sdram's
# op_addr seven picoseconds short, then dph_strb into ria_regs' write
# enables and dph_addr into the font store's strobe, twenty-six short —
# each of them physically holding by ninety-odd picoseconds and going
# negative only under the uncertainty stacked on the crossing above.
# The check is void wherever the payload lands, so it is cut at the
# clock and the lottery is out of tickets. Setup stays, and matters:
# the payload must still cross before the strobe does.
set_false_path -hold \
    -from [get_registers {*rv_soc*|dph_addr[*]}] \
    -to [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}]
set_false_path -hold \
    -from [get_registers {*rv_soc*|dph_strb[*]}] \
    -to [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}]

# The file bridge's command crosses the same way: the parameters stand
# still while a toggle carries the news, and only the toggle's first
# stage is cut by the rule above. Bound the parameters instead of
# cutting them, so a word cannot outlast the toggle that announced it.
# Named by source and stopped at the far clock, because the command
# parameters leave this block entirely — core_bridge_cmd's own target
# registers are where they land.
set_max_delay -from [get_registers {*pocket_file*|pocket_file_id[*]}] \
    -to [get_clocks {clk_74a}] 13.468
set_min_delay -from [get_registers {*pocket_file*|pocket_file_id[*]}] \
    -to [get_clocks {clk_74a}] 0
set_max_delay -from [get_registers {*pocket_file*|pocket_file_slotoffset[*]}] \
    -to [get_clocks {clk_74a}] 13.468
set_min_delay -from [get_registers {*pocket_file*|pocket_file_slotoffset[*]}] \
    -to [get_clocks {clk_74a}] 0
set_max_delay -from [get_registers {*pocket_file*|pocket_file_bridgeaddr[*]}] \
    -to [get_clocks {clk_74a}] 13.468
set_min_delay -from [get_registers {*pocket_file*|pocket_file_bridgeaddr[*]}] \
    -to [get_clocks {clk_74a}] 0
set_max_delay -from [get_registers {*pocket_file*|pocket_file_length[*]}] \
    -to [get_clocks {clk_74a}] 13.468
set_min_delay -from [get_registers {*pocket_file*|pocket_file_length[*]}] \
    -to [get_clocks {clk_74a}] 0
set_max_delay -from [get_registers {*pocket_file*|r_op[*]}] \
    -to [get_clocks {clk_74a}] 13.468
set_min_delay -from [get_registers {*pocket_file*|r_op[*]}] \
    -to [get_clocks {clk_74a}] 0

# The savestate result crosses the same way and needs the same bound:
# the machine writes a code and flips a toggle beside it, and the code
# has to be there when the toggle arrives three flops later.
set_max_delay -from [get_registers {*pocket_sst*|res_code[*]}] \
    -to [get_clocks {clk_74a}] 13.468
set_min_delay -from [get_registers {*pocket_sst*|res_code[*]}] \
    -to [get_clocks {clk_74a}] 0

# And the answer, coming back behind its own toggle.
set_max_delay -from [get_registers {*pocket_file*|err_q[*]}] \
    -to [get_registers {*pocket_file*|r_err[*]}] 13.468
set_min_delay -from [get_registers {*pocket_file*|err_q[*]}] \
    -to [get_registers {*pocket_file*|r_err[*]}] 0
set_max_delay -from [get_registers {*pocket_file*|tmo_q}] \
    -to [get_registers {*pocket_file*|tmo_flag}] 13.468
set_min_delay -from [get_registers {*pocket_file*|tmo_q}] \
    -to [get_registers {*pocket_file*|tmo_flag}] 0
set_max_delay -from [get_registers {*pocket_file*|result_q[*]}] \
    -to [get_registers {*pocket_file*|r_result[*]}] 13.468
set_min_delay -from [get_registers {*pocket_file*|result_q[*]}] \
    -to [get_registers {*pocket_file*|r_result[*]}] 0
