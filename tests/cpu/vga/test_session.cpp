/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/host.h"
#include "mut.h"
#include "session_prog.h"
#include "utest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

UTEST(session, scripted_terminal_frame_640x480)
{
    const int w = 640, h = 480;
    const size_t px = (size_t)w * (size_t)h;

    std::vector<uint8_t> rom;
    ASSERT_TRUE(session_rom(rom));
    const char *path = TEST_SCRATCH "/test_session.rp6502";
    ASSERT_TRUE(tb_rom_write(path, rom));
    ASSERT_TRUE(mut_boot(path));

    static uint32_t settled[640 * 480];
    memcpy(settled, mut_frame(w, h), px * sizeof(uint32_t));
    ASSERT_EQ(memcmp(settled, mut_frame(w, h), px * sizeof(uint32_t)), 0);

    uint32_t got = host_crc32(0, settled, px * sizeof(uint32_t));
    if (getenv("RP6502_BLESS_CRC"))
        fprintf(stderr, "session frame 0x%08X\n", got);
    else
        ASSERT_EQ(got, 0x1059759Eu);
}

MUT_MAIN()
