/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for the W65C02S disassembler (vendor/chips/util/w65c02dasm.h)
 * that drives the on-screen ui_dbg disassembly.
 * Each case feeds a hand-built byte sequence and checks both the decoded text
 * and the instruction length (so PC tracking stays aligned). No toolchain or
 * fixture file is needed. Focus is the CMOS additions and the documented
 * multi-byte NOP fills; a few NMOS opcodes guard against regressions.
 */

#define CHIPS_UTIL_IMPL
#include "chips/util/w65c02dasm.h"
#include "utest.h"

#include <string.h>
#include <stdint.h>

static const uint8_t *g_bytes;
static int g_idx;
static char *g_out;
static int g_outcap;
static int g_outlen;

static uint8_t dis_in(void *u)
{
    (void)u;
    return g_bytes[g_idx++];
}
static void dis_out(char c, void *u)
{
    (void)u;
    if (g_outlen + 1 < g_outcap)
        g_out[g_outlen++] = c;
}

/* Disassemble one instruction at pc; write text into out, return byte length. */
static int disasm(uint16_t pc, const uint8_t *bytes, char *out, int cap)
{
    g_bytes = bytes;
    g_idx = 0;
    g_out = out;
    g_outcap = cap;
    g_outlen = 0;
    uint16_t np = w65c02dasm_op(pc, dis_in, dis_out, NULL);
    out[g_outlen] = 0;
    return (int)(uint16_t)(np - pc);
}

#define CHECK(pc, len, text, ...)                       \
    do                                                  \
    {                                                   \
        const uint8_t b[] = {__VA_ARGS__};              \
        char buf[64];                                   \
        int n = disasm((pc), b, buf, (int)sizeof(buf)); \
        ASSERT_STREQ((text), buf);                      \
        ASSERT_EQ((len), n);                            \
    } while (0)

/* CMOS-only additions: the whole point of the fork. */
UTEST(w65c02dasm, cmos_new_ops)
{
    CHECK(0x1000, 2, "BRA $1000", 0x80, 0xFE); /* rel: 0x1002 + (-2) */
    CHECK(0x1000, 2, "BRA $1041", 0x80, 0x3F);
    CHECK(0x0000, 1, "INC A", 0x1A);
    CHECK(0x0000, 1, "DEC A", 0x3A);
    CHECK(0x0000, 1, "PHY", 0x5A);
    CHECK(0x0000, 1, "PLY", 0x7A);
    CHECK(0x0000, 1, "PHX", 0xDA);
    CHECK(0x0000, 1, "PLX", 0xFA);
    CHECK(0x0000, 1, "WAI", 0xCB);
    CHECK(0x0000, 1, "STP", 0xDB);
}

UTEST(w65c02dasm, cmos_stz_tsb_trb_bit)
{
    CHECK(0x0000, 2, "STZ $12", 0x64, 0x12);
    CHECK(0x0000, 2, "STZ $12,X", 0x74, 0x12);
    CHECK(0x0000, 3, "STZ $2000", 0x9C, 0x00, 0x20);
    CHECK(0x0000, 3, "STZ $2000,X", 0x9E, 0x00, 0x20);
    CHECK(0x0000, 2, "TSB $30", 0x04, 0x30);
    CHECK(0x0000, 3, "TSB $1234", 0x0C, 0x34, 0x12);
    CHECK(0x0000, 2, "TRB $30", 0x14, 0x30);
    CHECK(0x0000, 3, "TRB $1234", 0x1C, 0x34, 0x12);
    CHECK(0x0000, 2, "BIT #$55", 0x89, 0x55); /* CMOS BIT immediate */
    CHECK(0x0000, 2, "BIT $20,X", 0x34, 0x20);
    CHECK(0x0000, 3, "BIT $2000,X", 0x3C, 0x00, 0x20);
}

UTEST(w65c02dasm, cmos_zp_indirect)
{
    CHECK(0x0000, 2, "ORA ($20)", 0x12, 0x20);
    CHECK(0x0000, 2, "AND ($20)", 0x32, 0x20);
    CHECK(0x0000, 2, "EOR ($20)", 0x52, 0x20);
    CHECK(0x0000, 2, "ADC ($20)", 0x72, 0x20);
    CHECK(0x0000, 2, "STA ($20)", 0x92, 0x20);
    CHECK(0x0000, 2, "LDA ($20)", 0xB2, 0x20);
    CHECK(0x0000, 2, "CMP ($20)", 0xD2, 0x20);
    CHECK(0x0000, 2, "SBC ($20)", 0xF2, 0x20);
    CHECK(0x0000, 3, "JMP ($2000,X)", 0x7C, 0x00, 0x20); /* CMOS JMP (abs,X) */
}

UTEST(w65c02dasm, cmos_rmb_smb_bbr_bbs)
{
    CHECK(0x0000, 2, "RMB0 $10", 0x07, 0x10);
    CHECK(0x0000, 2, "RMB7 $10", 0x77, 0x10);
    CHECK(0x0000, 2, "SMB0 $10", 0x87, 0x10);
    CHECK(0x0000, 2, "SMB7 $10", 0xF7, 0x10);
    /* BBRx/BBSx: zp byte then a relative target (3 bytes) */
    CHECK(0x1000, 3, "BBR0 $10,$1000", 0x0F, 0x10, 0xFD); /* 0x1003 + (-3) */
    CHECK(0x1000, 3, "BBR7 $20,$1010", 0x7F, 0x20, 0x0D);
    CHECK(0x1000, 3, "BBS0 $10,$1000", 0x8F, 0x10, 0xFD);
    CHECK(0x1000, 3, "BBS7 $20,$1010", 0xFF, 0x20, 0x0D);
}

/* W65C02S documents the "unused" opcodes as NOPs of specific lengths. */
UTEST(w65c02dasm, documented_nop_fills)
{
    CHECK(0x0000, 1, "NOP", 0xEA);       /* the canonical NOP */
    CHECK(0x0000, 1, "NOP", 0x03);       /* 1-byte */
    CHECK(0x0000, 1, "NOP", 0xEB);       /* 1-byte (NMOS *SBC) */
    CHECK(0x0000, 2, "NOP #$12", 0x02, 0x12);  /* 2-byte imm */
    CHECK(0x0000, 2, "NOP #$12", 0xE2, 0x12);
    CHECK(0x0000, 2, "NOP $44", 0x44, 0x44);   /* 2-byte zp */
    CHECK(0x0000, 2, "NOP $44,X", 0x54, 0x44); /* 2-byte zp,X */
    CHECK(0x0000, 3, "NOP $2000", 0x5C, 0x00, 0x20); /* 3-byte abs */
    CHECK(0x0000, 3, "NOP $2000", 0xDC, 0x00, 0x20);
    CHECK(0x0000, 3, "NOP $2000", 0xFC, 0x00, 0x20);
}

/* Base/NMOS opcodes must still decode correctly (no regression). */
UTEST(w65c02dasm, base_ops_unchanged)
{
    CHECK(0x0000, 2, "LDA #$42", 0xA9, 0x42);
    CHECK(0x0000, 2, "LDX #$42", 0xA2, 0x42);
    CHECK(0x0000, 3, "JMP $2000", 0x4C, 0x00, 0x20);
    CHECK(0x0000, 3, "JMP ($2000)", 0x6C, 0x00, 0x20);
    CHECK(0x0000, 3, "JSR $2000", 0x20, 0x00, 0x20);
    CHECK(0x0000, 1, "RTS", 0x60);
    CHECK(0x0000, 1, "ASL A", 0x0A);
    CHECK(0x0000, 2, "ORA ($20,X)", 0x01, 0x20);
    CHECK(0x0000, 2, "ORA ($20),Y", 0x11, 0x20);
    CHECK(0x0000, 3, "STA $2000,X", 0x9D, 0x00, 0x20);
    CHECK(0x0000, 3, "LDA $2000,Y", 0xB9, 0x00, 0x20);
    CHECK(0x0000, 2, "LDX $20,Y", 0xB6, 0x20);
    CHECK(0x1000, 2, "BNE $0FF0", 0xD0, 0xEE); /* 0x1002 + (-16) */
}

/* Every one of the 256 opcodes must decode to a real mnemonic of valid length
 * (the W65C02S has no undefined opcodes -- all "unused" ones are NOPs). Guards
 * the bulk table against a typo'd or missing entry. */
UTEST(w65c02dasm, all_256_well_formed)
{
    for (int op = 0; op < 256; op++)
    {
        const uint8_t b[3] = {(uint8_t)op, 0x34, 0x12};
        char buf[64];
        int n = disasm(0x4000, b, buf, (int)sizeof(buf));
        ASSERT_TRUE(n >= 1 && n <= 3);                /* 1..3 byte length */
        ASSERT_TRUE(buf[0] >= 'A' && buf[0] <= 'Z');  /* a real mnemonic */
        ASSERT_TRUE(strchr(buf, '?') == NULL);        /* never "???" */
    }
}

UTEST_MAIN()
