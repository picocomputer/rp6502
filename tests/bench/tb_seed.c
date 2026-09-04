/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The bench is a machine, so it owes host_seed like any other -- and
 * being a bench, it answers a fixed one. Every test therefore runs the same
 * stream, which is what lets an oracle be written down; a test that wants a
 * different stream sets its own state through sys_random_step.
 *
 * Linked into every test rather than named at each call site: the symbol is
 * wanted by whatever pulls core/api/attr.c, which is most of them.
 */

#include "core/sys/random.h"

#include <stdint.h>

uint32_t host_seed(void)
{
    return 0x6502C0DE;
}
