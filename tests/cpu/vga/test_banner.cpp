/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/host.h"
#include "mut.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const char banner[] =
    "\33[0m\33[2J\33[H\33[?25l"
    "RP6502 openFPGA\r\n"
    "\33[31mred\33[32m green \33[1;34mbold blue\33[0m\r\n"
    "\33[7mreverse\33[0m \33[4munderline\33[0m plain\r\n";

UTEST(banner, ansi_banner_frame_640x480)
{
    /* prog: ldx #0; loop: lda msg,x; beq done; wait: bit $FFE0; bpl wait;
     * sta $FFE1; inx; bne loop; nop; done: stp; msg follows. */
    std::vector<uint8_t> prog = {
        0xA2, 0x00,
        0xBD, 0x14, 0x03,
        0xF0, 0x0C,
        0x2C, 0xE0, 0xFF,
        0x10, 0xFB,
        0x8D, 0xE1, 0xFF,
        0xE8,
        0xD0, 0xF0,
        0xEA,
        0xDB,
    };
    prog.insert(prog.end(), banner, banner + sizeof(banner));
    static const uint8_t vectors[] = {0x00, 0x03};

    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, 0x0300, prog.data(), prog.size());
    tb_rom_record(rom, 0xFFFC, vectors, sizeof(vectors));

    const int w = 640, h = 480;
    const size_t px = (size_t)w * (size_t)h;

    const char *path = TEST_SCRATCH "/test_banner.rp6502";
    ASSERT_TRUE(tb_rom_write(path, rom));
    ASSERT_TRUE(mut_boot(path));

    static uint32_t settled[640 * 480];
    memcpy(settled, mut_frame(w, h), px * sizeof(uint32_t));
    ASSERT_EQ(memcmp(settled, mut_frame(w, h), px * sizeof(uint32_t)), 0);

    uint32_t got = host_crc32(0, settled, px * sizeof(uint32_t));
    if (getenv("RP6502_BLESS_CRC"))
        fprintf(stderr, "banner frame 0x%08X\n", got);
    else
        ASSERT_EQ(got, 0x386617E0u);
}

MUT_MAIN()
