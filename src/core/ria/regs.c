/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/ria/regs.h"
#include "machine.h"
#include <stdalign.h>

alignas(0x20) volatile uint8_t HOST_REGISTER_RAM(regs)[0x20];

uint8_t xstack[XSTACK_SIZE + 1];
volatile uint16_t xstack_ptr = XSTACK_SIZE;

/* The byte above the top is always zero, because api_pop_str runs strlen up
 * to it. */
static bool regs_sst_valid(sst_cursor_t *c)
{
    if (xstack_ptr > XSTACK_SIZE || xstack[XSTACK_SIZE])
        return sst_refuse(c, "an xstack out of range");
    return true;
}

/* regs is volatile, so it goes a byte at a time through the volatile lvalue.
 * BYTES would copy it through a plain pointer, which is undefined. */
#define REGS_CELL_ROWS(Y, r) Y(U8, r)
#define REGS_SST_ROWS(X)                                        \
    X(STRUCTS, regs, REGS_CELL_ROWS)                            \
    X(BYTES, xstack)                                            \
    X(U16, xstack_ptr)                                          \
    X(VALIDATE, regs_sst_valid)
SST_TABLE(regs, REGS_SST_ROWS)

void regs_reset(void)
{
    xstack_ptr = XSTACK_SIZE;
    xstack[XSTACK_SIZE] = 0;
}
