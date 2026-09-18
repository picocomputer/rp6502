/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_WDC_RTL_DUT_H_
#define _TESTS_WDC_RTL_DUT_H_

#include "dut.h"

extern const dut_t rtl_dut;

void rtl_dut_init(int argc, const char *const argv[]);
void rtl_dut_free(void);

#endif /* _TESTS_WDC_RTL_DUT_H_ */
