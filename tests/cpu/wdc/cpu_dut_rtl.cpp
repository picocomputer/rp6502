/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu_dut.h"

#include "rtl_dut.h"

const dut_t *const cpu_dut = &rtl_dut;

void cpu_dut_init(int argc, const char *const argv[])
{
    rtl_dut_init(argc, argv);
}

void cpu_dut_free(void)
{
    rtl_dut_free();
}
