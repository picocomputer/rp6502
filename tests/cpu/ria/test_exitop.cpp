/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * EXIT ($FF in RIA_OP) stops the 6502 before it runs another instruction, so
 * the store of '!' right after it never happens. The C runtimes count on
 * this: neither spins after it.
 */

#include "mut.h"
#include "tb_asm.h"
#include "tb_rom.h"
#include "utest.h"

#include <string.h>
#include <vector>

UTEST(exitop, exit_stops_the_6502_at_once)
{
    tb_asm p;
    p.store(TB_RIA_TX, 'A');
    p.store(TB_API_OP, 0xFF);
    p.store(TB_RIA_TX, '!');
    p.stp();
    std::vector<uint8_t> rom = tb_rom_image(TB_ORG, p.b);
    const char *path = TEST_SCRATCH "/test_exitop.rp6502";
    ASSERT_TRUE(tb_rom_write(path, rom));

    mut_console_start();
    ASSERT_TRUE(mut_boot(path));
    size_t len;
    const char *out = mut_console(&len);
    ASSERT_GE(len, (size_t)1);
    ASSERT_EQ(out[0], 'A');
    ASSERT_TRUE(memchr(out, '!', len) == NULL);
}

MUT_MAIN()
