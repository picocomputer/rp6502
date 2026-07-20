/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "ria/sys/sys.h"
#include "pico/time.h"

/* Master-clock time (us) at the current program's start. */
static uint64_t sys_start_us;

void sys_run(void)
{
    sys_start_us = time_us_64();
}

/* 6502 run time from the one master clock, so it is a reproducible function of
 * the frame count (the vendored atr.c reads it for the s/ds/cs/ms attributes). */
uint32_t sys_get_run(uint32_t us_per_tick)
{
    return (uint32_t)((time_us_64() - sys_start_us) / us_per_tick);
}
