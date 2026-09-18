/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_FPGA_TB_CORE_H_
#define _TESTS_FPGA_TB_CORE_H_

#include <cstdint>

void tb_core_args(int argc, const char *const argv[]);

void tb_core_init();

void tb_core_free();

void tb_core_reset();

void tb_core_clocks(int count);

uint16_t tb_core_scanline();

uint16_t tb_core_h();
bool tb_core_hsync();
bool tb_core_vsync();
bool tb_core_de();

#endif /* _TESTS_FPGA_TB_CORE_H_ */
