/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The engine, with its trace hook reporting each write as it is applied. */

#include <stdint.h>

void opl_trace(uint8_t reg, uint8_t val);
#define OPL_TRACE(reg, val) opl_trace((uint8_t)(reg), (uint8_t)(val))

#include "core/aud/opl.c"

/* Slots that re-evaluate the whole rule in the next sample. */
int opl_forced_slots(void)
{
    int n = 0;
    for (int i = 0; i < 18; i++)
        n += !opl_chip.sl[i].span;
    return n;
}
