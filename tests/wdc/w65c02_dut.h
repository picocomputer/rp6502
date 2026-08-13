/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The verilated w65c02 as a dut_t, shared by the conformance and lockstep
 * tests. Owns the single model instance.
 */

#ifndef _TESTS_WDC_W65C02_DUT_H_
#define _TESTS_WDC_W65C02_DUT_H_

#include "dut.h"

extern const dut_t w65c02_dut;

/* Construct the model and run the power-on reset. Once per process. */
void w65c02_dut_init(int argc, const char *const argv[]);
void w65c02_dut_free(void);

#endif /* _TESTS_WDC_W65C02_DUT_H_ */
