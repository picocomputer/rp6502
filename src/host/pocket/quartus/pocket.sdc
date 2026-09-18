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

set_false_path -to [get_registers {*pocket_fifo*|wptr_gray_r1[*]}]
set_false_path -to [get_registers {*pocket_fifo*|rptr_gray_w1[*]}]

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

# general[0] is clk_sys, and general[1] is both clk_rv and clk_vid,
# because Quartus merges those two identical PLL outputs into one
# counter. clk_mach is clk_sys gated by gate_sys, the clkctrl cell in
# core_top, and the analyzer propagates general[0] through that cell, so
# every constraint here that names general[0] also applies to the
# registers on clk_mach.
set_clock_uncertainty -add -hold 0.060 \
    -from [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}] \
    -to [get_clocks {*|general[1].gpll~PLL_OUTPUT_COUNTER|divclk}]
set_clock_uncertainty -add -hold 0.060 \
    -from [get_clocks {*|general[1].gpll~PLL_OUTPUT_COUNTER|divclk}] \
    -to [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}]

set_clock_uncertainty -add -hold 0.080 \
    -from [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}] \
    -to [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}]
set_clock_uncertainty -add -hold 0.080 \
    -from [get_clocks {bridge_spiclk}] \
    -to [get_clocks {bridge_spiclk}]

set_false_path -hold \
    -from [get_registers {*|opl:*|reset_sync:*|r2}] \
    -to [get_registers {*|opl:*|afifo:*}]

# soc loads dph_addr and dph_strb only while hready is high, and hready
# is the inverse of soc_bus_pend, so neither register changes on a clk_rv
# edge at which soc_bus_pend is high. wiring and pocket_core use them on
# clk_sys only under soc_bus_pend or a strobe derived from it, so no
# clk_sys capture that is used falls on an edge where they change.
set_false_path -hold \
    -from [get_registers {*soc*|dph_addr[*]}] \
    -to [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}]
set_false_path -hold \
    -from [get_registers {*soc*|dph_strb[*]}] \
    -to [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}]

# In hazard3_cpu_1port.v, bus_active_dph_i, bus_active_dph_d and
# bus_active_dph_s are loaded outside reset only while hready is high,
# the same condition under which soc loads dph_addr. bus_active_dph_s
# selects the source of hwdata, which wiring and pocket_core also use on
# clk_sys only under soc_bus_pend or a strobe derived from it.
set_false_path -hold \
    -from [get_registers {*hazard3_cpu_1port*|bus_active_dph_*}] \
    -to [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}]

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
# requested index into ask on the clk_74a edge that flips ask_t, and
# sst_engine loads req_idx from it only after ask_t has passed two
# synchronizer flops. sst_engine raises rvalid one clk_sys clock after it
# loads hold, and pocket_sst loads its hold register only after rvalid
# has passed two synchronizer flops. Each path is bounded to one clk_74a
# period rather than cut, so each value is stable before it is loaded.
set_max_delay -from [get_registers {*pocket_sst*|ask[*]}] \
    -to [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}] 13.468
set_min_delay -from [get_registers {*pocket_sst*|ask[*]}] \
    -to [get_clocks {*|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}] 0
set_max_delay -from [get_registers {*sst_engine*|hold[*]}] \
    -to [get_registers {*pocket_sst*|hold[*]}] 13.468
set_min_delay -from [get_registers {*sst_engine*|hold[*]}] \
    -to [get_registers {*pocket_sst*|hold[*]}] 0

# mach_clk_en is a clk_74a register. The clock gate takes it into an
# enable register inside the clkctrl cell on the falling edge of clk_sys,
# and every other reader passes it through an _s1 synchronizer first.
set_false_path -from [get_registers {*|mach_clk_en}]

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
