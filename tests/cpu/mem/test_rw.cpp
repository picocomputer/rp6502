/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mut.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

UTEST(rw, steps_wraps_and_defaults)
{
    std::vector<uint8_t> p;
    auto lda = [&](uint8_t v) { p.insert(p.end(), {0xA9, v}); };
    auto sta = [&](uint16_t a) {
        p.insert(p.end(), {0x8D, (uint8_t)a, (uint8_t)(a >> 8)});
    };
    auto ldaa = [&](uint16_t a) {
        p.insert(p.end(), {0xAD, (uint8_t)a, (uint8_t)(a >> 8)});
    };
    auto set = [&](uint16_t reg, uint8_t v) { lda(v); sta(reg); };
    auto rw0 = [&](uint8_t v) { set(0xFFE4, v); };
    auto rw1 = [&](uint8_t v) { set(0xFFE8, v); };
    auto rd0 = [&]() { ldaa(0xFFE4); sta(0xFFE1); };
    auto rd1 = [&]() { ldaa(0xFFE8); sta(0xFFE1); };

    rw0('A'); rw0('B'); rw0('C');
    set(0xFFEA, 0); set(0xFFEB, 0); set(0xFFE9, 1);
    rd1(); rd1(); rd1();
    set(0xFFE5, 0xFF); set(0xFFE6, 0x05); set(0xFFE7, 0x00);
    rw0('X'); rw0('Y');
    set(0xFFEA, 4); set(0xFFEB, 0); set(0xFFE9, 1);
    rd1(); rd1();
    set(0xFFE5, 1); set(0xFFE6, 0xFF); set(0xFFE7, 0xFF);
    rw0('W'); rw0('Z');
    set(0xFFEA, 0xFF); set(0xFFEB, 0xFF);
    rd1(); rd1();
    set(0xFFE5, 0); set(0xFFE6, 8); set(0xFFE7, 0);
    rw0('Q'); rw0('R');
    set(0xFFEA, 8); set(0xFFEB, 0); set(0xFFE9, 0);
    rd1(); rd1();
    set(0xFFE5, 127); set(0xFFE6, 0x00); set(0xFFE7, 0x01);
    rw0('a'); rw0('b'); rw0('c');
    set(0xFFEA, 0x00); set(0xFFEB, 0x01); set(0xFFE9, 127);
    rd1(); rd1(); rd1();
    set(0xFFE5, 1); set(0xFFE6, 2); set(0xFFE7, 0);
    rd0();
    set(0xFFEA, 0x20); set(0xFFEB, 0); set(0xFFE9, 1);
    rw1('D'); rw1('E');
    set(0xFFE6, 0x20); set(0xFFE7, 0);
    rd0(); rd0();
    /* RW0's address is already $22 when 'F' is written there through RW1, so
     * a read of RW0 returns 'F' only if RW0's byte is fetched again after the
     * write. */
    rw1('F');
    rd0();
    p.push_back(0xDB); /* stp */

    static const uint8_t vectors[] = {0x00, 0x03};
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, 0x0300, p.data(), p.size());
    tb_rom_record(rom, 0xFFFC, vectors, sizeof(vectors));

    const char *path = TEST_SCRATCH "/test_rw.rp6502";
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fwrite(rom.data(), 1, rom.size(), f);
    fclose(f);

    mut_console_start();
    ASSERT_TRUE(mut_boot(path));

    static const char want[] = "ABCYXWZRRabcCDEF";
    size_t len;
    const char *out = mut_console(&len);
    ASSERT_EQ(len, sizeof want - 1);
    ASSERT_EQ(memcmp(out, want, len), 0);
}

MUT_MAIN()
