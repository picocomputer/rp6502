/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The verilated cpu65 as a dut_t, shared by the conformance and lockstep
 * tests. Owns the single model instance.
 */

#ifndef _TESTS_FPGA_CPU65_DUT_H_
#define _TESTS_FPGA_CPU65_DUT_H_

#include "dut.h"

extern const dut_t cpu65_dut;

/* Construct the model and run the power-on reset. Once per process. */
void cpu65_dut_init(int argc, const char *const argv[]);
void cpu65_dut_free(void);

#endif /* _TESTS_FPGA_CPU65_DUT_H_ */
