/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Testbench around the verilated rp6502 machine. Single instance, like the chip
 * models in core — the machine is a singleton in a test process too.
 *
 * Set RP6502_RTL_TRACE to a path to write an FST trace of the run.
 */

#ifndef _TESTS_FPGA_TB_CORE_H_
#define _TESTS_FPGA_TB_CORE_H_

#include <cstdint>

/* Hand argv to Verilator (plusargs). Tests never include verilated.h itself —
 * it and utest.h both define __STDC_FORMAT_MACROS. */
void tb_core_args(int argc, const char *const argv[]);

/* Construct the model and hold it in reset. */
void tb_core_init();

/* Release the model and close any trace. */
void tb_core_free();

/* Assert reset for a few clocks, then release it. */
void tb_core_reset();

/* Advance count full clk_sys cycles. */
void tb_core_clocks(int count);

uint16_t tb_core_scanline();

/* The raster as the beam sees it: pixel column, sync levels, active video. */
uint16_t tb_core_h();
bool tb_core_hsync();
bool tb_core_vsync();
bool tb_core_de();

#endif /* _TESTS_FPGA_TB_CORE_H_ */
