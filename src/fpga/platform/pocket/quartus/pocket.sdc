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
# time the reader looks. Bound it rather than cut it: a word must not
# take longer to cross than the pointer that announces it.
set_max_delay -from [get_registers {*pocket_fifo*|mem*}] \
    -to [get_registers {*pocket_fifo*}] 13.468
set_min_delay -from [get_registers {*pocket_fifo*|mem*}] \
    -to [get_registers {*pocket_fifo*}] 0

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
