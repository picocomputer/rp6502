/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The CPU under test, when it is the emulator's.
 *
 * chips_dut.c is a plain C model with no lifecycle of its own, so the two
 * halves of this file are a pointer and two empty functions. It exists for
 * the same reason its verilated counterpart does: so a suite can name one
 * CPU and get whichever this tree built.
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
