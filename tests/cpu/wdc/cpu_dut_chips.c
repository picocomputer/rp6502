/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu_dut.h"

#include "chips_dut.h"

const dut_t *const cpu_dut = &chips_dut;

void cpu_dut_init(int argc, const char *const argv[])
{
    (void)argc;
    (void)argv;
}

void cpu_dut_free(void)
{
}
