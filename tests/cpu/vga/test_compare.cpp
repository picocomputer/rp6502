/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mut.h"
#include "tb_rom.h"
#include "utest.h"

#include <stdio.h>
#include <stdlib.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const uint8_t prog[] = {
    0xA9, 0x41,       /* lda #'A'      */
    0x8D, 0xE1, 0xFF, /* sta $FFE1     */
    0xA9, 0x43,       /* lda #'C'      */
    0x8D, 0xEC, 0xFF, /* sta $FFEC     */
    0xA9, 0x42,       /* lda #'B'      */
    0x8D, 0xEC, 0xFF, /* sta $FFEC     */
    0xA9, 0x01,       /* lda #1        */
    0x8D, 0xF4, 0xFF, /* sta $FFF4     */
    0xA9, 0x00,       /* lda #0        */
    0x8D, 0xF6, 0xFF, /* sta $FFF6     */
    0xA9, 0x18,       /* lda #$18      */
    0x8D, 0xEF, 0xFF, /* sta $FFEF     */
    0x20, 0xF1, 0xFF, /* jsr $FFF1     */
    0x8D, 0xE1, 0xFF, /* sta $FFE1     */
    0xA9, 0x35,       /* lda #$35      */
    0x8D, 0xEF, 0xFF, /* sta $FFEF     */
    0x20, 0xF1, 0xFF, /* jsr $FFF1     */
    0x8D, 0xE1, 0xFF, /* sta $FFE1     */
    0x8E, 0xE1, 0xFF, /* stx $FFE1     */
    0xAD, 0xED, 0xFF, /* lda $FFED     */
    0x8D, 0xE1, 0xFF, /* sta $FFE1     */
    0xAD, 0xEE, 0xFF, /* lda $FFEE     */
    0x8D, 0xE1, 0xFF, /* sta $FFE1     */
    0xDB,             /* stp           */
};
static const uint8_t vectors[] = {0x00, 0x03};

UTEST(compare, syscall_abi_console_stream)
{
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, 0x0300, prog, sizeof(prog));
    tb_rom_record(rom, 0xFFFC, vectors, sizeof(vectors));

    const char *path = TEST_SCRATCH "/test_compare.rp6502";
    ASSERT_TRUE(tb_rom_write(path, rom));

    mut_console_start();
    ASSERT_TRUE(mut_boot(path));

    /* 'B' is pushed after 'C', so it is on top of the xstack and is written
     * first. */
    static const char want[] = {0x41, 0x42, 0x43, 0x02,
                                (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF};
    size_t len;
    const char *out = mut_console(&len);
    ASSERT_EQ(len, sizeof want);
    ASSERT_EQ(memcmp(out, want, len), 0);
}

MUT_MAIN()