/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * exec.rp6502 writes 1 to XRAM $0000 when it starts with only argv[0], then
 * executes itself again with "Foo". The second run writes 2 and argv[1] from
 * $0001, so the six bytes show that both runs happened and that the argument
 * arrived.
 */

#include "mut.h"
#include "utest.h"

#include <cstring>

UTEST(exec, reexecs_self_with_arg)
{
    ASSERT_TRUE(mut_boot(EXEC_ROM));
    static const uint8_t want[] = {1, 2, 'F', 'o', 'o', 0};
    uint8_t got[sizeof want];
    mut_xram(0, got, sizeof got);
    ASSERT_EQ(memcmp(got, want, sizeof want), 0);
}

MUT_MAIN()
