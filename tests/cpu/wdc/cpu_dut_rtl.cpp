/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The CPU under test, when it is the fabric's.
 *
 * rtl_dut.cpp owns the model and answers dut.h; this points the suites at
 * it and forwards the lifecycle it needs — a verilated model has to be
 * constructed, and Verilator wants the command line before anything else
 * happens.
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
