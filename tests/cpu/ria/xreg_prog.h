/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_CPU_RIA_XREG_PROG_H_
#define _TESTS_CPU_RIA_XREG_PROG_H_

#include "tb_asm.h"
#include "tb_rom.h"

#include <vector>

static void xreg_rom(std::vector<uint8_t> &rom)
{
    tb_asm p;
    auto push = [&](uint8_t v) { p.push(v); };
    auto pushw = [&](uint16_t w) { p.pushw(w); };
    auto op1 = [&]() {
        p.call(0x01);
        p.put_ax();
        p.put_errno();
    };
    auto opn = [&](uint8_t op, uint8_t a) {
        p.call_a(op, a);
        p.put_ax();
        p.put_errno();
    };
    auto opn_errno = [&](uint8_t op, uint8_t a) {
        p.call_a(op, a);
        p.put_errno();
    };
    auto differ = [&](uint16_t first) {
        p.cmp_abs(first);
        p.beq(2);
        p.ldx(1);
    };

    /* ATTR_ERRNO_OPT is set first. Every errno is -1 until a program selects
     * an errno map, so without it every failed call below would print the
     * same errno. */
    push(0); push(0); push(0); push(1); /* cc65 */
    opn(0x0B, 0);

    /* The ATTR_CLK_RUN_* reads come before any call that fails. API_ERRNO is
     * set to 0xFFFF when a program starts and a successful call leaves it
     * unchanged, so this is the only point where 0xFFFF shows that none of
     * the four reads failed. Only the errno is printed, because the value a
     * read returns depends on the machine. The Pocket firmware reads the run
     * clock from the MTIME counter, and the emulator computes it from the
     * scanline count. */
    opn_errno(0x0A, 0x10);
    opn_errno(0x0A, 0x11);
    opn_errno(0x0A, 0x12);
    opn_errno(0x0A, 0x13);

    /* A program's write to VGA channel 15, the control channel, fails with
     * EACCES. */
    push(1); push(15); push(0); pushw(0);
    op1();
    /* Device 8 is out of range, so the call fails with EINVAL. */
    push(8); push(0); push(0); pushw(0);
    op1();
    /* An extra byte misaligns the xstack, so the call fails with EINVAL. */
    push(1); push(0); push(0); pushw(0); push(0xAA);
    op1();
    /* Canvas 1 is 320x240. */
    push(1); push(0); push(0); pushw(1);
    op1();
    /* Mode 3 is set to 1bpp with its config at $1000, on plane 0 over the
     * whole canvas. */
    push(1); push(0); push(1);
    pushw(3); pushw(0); pushw(0x1000); pushw(0); pushw(0); pushw(0);
    op1();
    /* Mode 4 is set to plain sprites with 3 descriptors at $2000, on
     * plane 1. */
    push(1); push(0); push(1);
    pushw(4); pushw(0); pushw(0x2000); pushw(3); pushw(1);
    pushw(0); pushw(0);
    op1();
    /* Mode 4 has no attribute 2, so the call fails with EINVAL. */
    push(1); push(0); push(1);
    pushw(4); pushw(2); pushw(0x2000); pushw(3); pushw(1);
    pushw(0); pushw(0);
    op1();
    /* Mode 5 is set to 4bpp 16x16 sprites with 2 descriptors at $3000, on
     * plane 2. */
    push(1); push(0); push(1);
    pushw(5); pushw(10); pushw(0x3000); pushw(2); pushw(2);
    pushw(0); pushw(0);
    op1();

    /* Device 0, channel 1, register 0 is the PSG pointer. */
    push(0); push(1); push(0); pushw(0x9000);
    op1();
    push(0); push(1); push(0); pushw(0x9001); /* odd */
    op1();
    push(0); push(1); push(0); pushw(0xFFC2); /* runs past the end of XRAM */
    op1();
    push(0); push(1); push(0); pushw(0x90C2); /* crosses its page */
    op1();
    push(0); push(1); push(0); pushw(0x8000);
    op1();

    /* Register 1 on the same channel is the OPL pointer, which must be page
     * aligned. */
    push(0); push(1); push(1); pushw(0xF001); /* not page aligned */
    op1();
    push(0); push(1); push(1); pushw(0xF000);
    op1();

    /* Attribute 5 is ATTR_BEL, where 0 mutes the bell and 1 unmutes it. */
    opn(0x0A, 5);
    push(0); push(0); push(0); push(0);
    opn(0x0B, 5);
    opn(0x0A, 5);
    p.store(TB_RIA_TX, 0x07); /* BEL, muted */
    push(0); push(0); push(0); push(2); /* out of range */
    opn(0x0B, 5);
    push(0); push(0); push(0); push(1);
    opn(0x0B, 5);
    p.store(TB_RIA_TX, 0x07); /* BEL, rings */

    /* Attribute 2 is ATTR_CODE_PAGE. Neither the emulator nor the Pocket
     * firmware supports page 1252, so setting it leaves the page unchanged
     * and still succeeds. The page is read only after it is set, so the
     * printed output does not depend on the page each machine boots with. */
    push(0); push(0); push(3); push(0x52); /* 850 */
    opn(0x0B, 2);
    opn(0x0A, 2);
    push(0); push(0); push(4); push(0xE4); /* 1252 */
    opn(0x0B, 2);
    opn(0x0A, 2);
    push(0); push(0); push(1); push(0xB5); /* 437 */
    opn(0x0B, 2);
    opn(0x0A, 2);

    /* Attributes 7 and 8 are ATTR_EXIT_CODE and ATTR_SIGINT. */
    opn(0x0A, 7);
    opn(0x0A, 8);

    /* Attribute 1 is ATTR_PHI2_KHZ. The emulator and the Pocket firmware both
     * clamp 9000 to PHI2_MAX_KHZ, which is also the default, so the last set
     * puts back the rate the program started at. */
    opn(0x0A, 1);
    p.pushl(1000);
    opn(0x0B, 1);
    opn(0x0A, 1);
    p.pushl(9000);
    opn(0x0B, 1);
    opn(0x0A, 1);

    /* Attribute 4 is ATTR_LRAND. The program compares two draws and prints
     * only whether they differ, because the emulator bench and the Pocket
     * firmware seed the generator with different values. The generator has
     * full period and its output function is a bijection, so two consecutive
     * draws always differ before the 31-bit mask, and match after it only when
     * bit 31 is their sole difference. */
    p.call_a(0x0A, 4);
    p.sta(0x0200);
    p.stx(0x0201);
    p.lda_abs(TB_API_SREG);
    p.sta(0x0202);
    p.lda_abs(TB_API_SREG + 1);
    p.sta(0x0203);
    p.put_errno();
    p.call_a(0x0A, 4);
    p.stx(0x0204);
    p.ldx(0);
    differ(0x0200);
    p.lda_abs(0x0204);
    differ(0x0201);
    p.lda_abs(TB_API_SREG);
    differ(0x0202);
    p.lda_abs(TB_API_SREG + 1);
    differ(0x0203);
    p.put_x();
    p.pushl(0);
    opn(0x0B, 4);

    /* Attribute 3 is ATTR_RLN_LENGTH. */
    opn(0x0A, 3);
    p.pushl(0);
    opn(0x0B, 3);
    opn(0x0A, 3);
    p.pushl(0x100);
    opn(0x0B, 3);
    p.pushl(254);
    opn(0x0B, 3);
    p.stp();

    rom = tb_rom_image(TB_ORG, p.b);
}

#endif /* _TESTS_CPU_RIA_XREG_PROG_H_ */
