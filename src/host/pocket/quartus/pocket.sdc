# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# pocket_fifo passes its read and write pointers between clocks in gray
# code through two-flop synchronizers. No clock group separates the PLL's
# outputs from clk_74a or from each other, so the analyzer times every
# path into the first flop of each synchronizer.
#
# Each pointer moves at most one step per clock, so only one bit of its
# gray code changes at a time. A first flop that samples the pointer
# while it changes holds either the old value or the new one as long as
# the skew between its bits stays under one period of the clock that
# launches it, and either value keeps the full and empty flags
# conservative.

set_false_path -to [get_registers {*pocket_fifo*|wr_gray_r1[*]}]
set_false_path -to [get_registers {*pocket_fifo*|rd_gray_w1[*]}]

set_false_path -to [get_registers {*synch_3*|stage_1[*]}]

# Every register whose name ends in _s1 or _t1 is the first flop of a
# two-flop synchronizer.
set_false_path -to [get_registers {*|*_s1}]
set_false_path -to [get_registers {*|*_s1[*]}]
set_false_path -to [get_registers {*|*_t1}]

# Outside reset, slot_size changes only on the clk_74a edge that flips
# settle_t, and it is loaded into pocket_bridge_slot_len only after
# settle_t has passed two synchronizer flops. The path is bounded to one
# clk_74a period rather than cut, so slot_size is stable before that
# load.
set_max_delay -from [get_registers {*pocket_bridge*|slot_size[*]}] \
    -to [get_registers {*pocket_bridge*|pocket_bridge_slot_len[*]}] 13.468
set_min_delay -from [get_registers {*pocket_bridge*|slot_size[*]}] \
    -to [get_registers {*pocket_bridge*|pocket_bridge_slot_len[*]}] 0

# soc loads mmio_slot_len from pocket_bridge_slot_len only while its
# slot_set input is high. pocket_bridge raises pocket_bridge_slot_set
# four clk_sys clocks after each load of pocket_bridge_slot_len made
# while pocket_bridge_run is high, and five clk_sys clocks after
# pocket_bridge_run rises, so a load of mmio_slot_len on an edge where
# pocket_bridge_slot_len changes is followed by another pulse of
# pocket_bridge_slot_set once pocket_bridge_run is high.
set_false_path -hold \
    -from [get_registers {*pocket_bridge*|pocket_bridge_slot_len[*]}] \
    -to [get_registers {*|mmio_slot_len[*]}]

# pocket_file's command parameters and r_op are read on clk_74a only
# after go_t has passed two synchronizer flops, and r_op changes on the
# same clk_sys edge that flips go_t. Each path is bounded to one clk_74a
# period rather than cut, so a value written with or before the toggle is
# stable when it is read. The bounds end at clk_74a rather than at a
# register because core_bridge_cmd also registers the parameters.
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

# The savestate read crosses in both directions. pocket_sst writes the
# requested index into ask on the clk_74a edge that flips ask_t, and its
# clk_sys side loads fetch_idx from it only after ask_t has passed two
# synchronizer flops. That side loads word on the clk_sys edge that flips
# got_t, and the clk_74a side loads hold from it only after got_t has
# passed two synchronizer flops. The firmware's answers cross the same
# way: save_err and load_err change on the clk_sys edge that flips
# save_ans_t or load_ans_t, and are read on clk_74a only after that flip
# has passed two synchronizer flops. Each path is bounded to one clk_74a
# period rather than cut, so each value is stable before it is loaded.
set_max_delay -from [get_registers {*pocket_sst*|ask[*]}] \
    -to [get_registers {*pocket_sst*|fetch_idx[*]}] 13.468
set_min_delay -from [get_registers {*pocket_sst*|ask[*]}] \
    -to [get_registers {*pocket_sst*|fetch_idx[*]}] 0
set_max_delay -from [get_registers {*pocket_sst*|word[*]}] \
    -to [get_registers {*pocket_sst*|hold[*]}] 13.468
set_min_delay -from [get_registers {*pocket_sst*|word[*]}] \
    -to [get_registers {*pocket_sst*|hold[*]}] 0
set_max_delay -from [get_registers {*pocket_sst*|save_err}] \
    -to [get_registers {*pocket_sst*|save_bad}] 13.468
set_min_delay -from [get_registers {*pocket_sst*|save_err}] \
    -to [get_registers {*pocket_sst*|save_bad}] 0
set_max_delay -from [get_registers {*pocket_sst*|load_err}] \
    -to [get_registers {*pocket_sst*|load_ok *pocket_sst*|load_bad}] 13.468
set_min_delay -from [get_registers {*pocket_sst*|load_err}] \
    -to [get_registers {*pocket_sst*|load_ok *pocket_sst*|load_bad}] 0

# pocket_file copies err_q, tmo_q, result_q and wrote_q into its clk_sys
# registers only after ret_t has passed two synchronizer flops. Each path
# is bounded to one clk_74a period rather than cut, so each value is
# stable before it is copied.
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
set_max_delay -from [get_registers {*pocket_file*|wrote_q}] \
    -to [get_registers {*pocket_file*|wrote_flag}] 13.468
set_min_delay -from [get_registers {*pocket_file*|wrote_q}] \
    -to [get_registers {*pocket_file*|wrote_flag}] 0
