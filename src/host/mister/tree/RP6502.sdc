# Copyright (c) 2026 Rumbledethumps
#
# SPDX-License-Identifier: BSD-3-Clause
#
# The core's own constraints. sys/sys_top.sdc derives every clock, the
# outputs of rtl/pll among them, and puts the three in one clock group,
# apart from sys/'s own clocks. Nothing crosses from clk_sys into
# CLK_VIDEO, which is clk_sys, so the colour bars need nothing here.
