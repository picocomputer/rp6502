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
