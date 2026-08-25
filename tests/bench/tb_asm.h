/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Enough 65C02 to write a test program, and no more. What the benches
 * need is a few loads and stores, the xstack, the API trampoline and a
 * way to say something on the console; assembling anything else would
 * be a feature nothing asks for.
 *
 * Addressing is absolute throughout. A test ROM lives at $0300 and
 * talks to $FFxx, so zero page would save bytes nobody is counting and
 * cost a second spelling of every instruction.
 *
 * tests/gen/rp6502_rom.py is the same instruction set in Python. They are
 * two spellings of one machine, so a program written against either
 * reads the same.
 */

#ifndef _TESTS_BENCH_TB_ASM_H_
#define _TESTS_BENCH_TB_ASM_H_

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

/* Where a test ROM is loaded and started. */
#define TB_ORG 0x0300

/* The RIA register window, as the 6502 sees it. */
#define TB_RIA_READY 0xFFE0
#define TB_RIA_TX 0xFFE1
#define TB_RIA_RX 0xFFE2
#define TB_RW0_DATA 0xFFE4
#define TB_RW0_ADDR 0xFFE6
#define TB_XSTACK 0xFFEC
#define TB_API_ERRNO 0xFFED
#define TB_API_OP 0xFFEF
#define TB_API_CALL 0xFFF1
#define TB_API_A 0xFFF4
#define TB_API_X 0xFFF6

struct tb_asm
{
    std::vector<uint8_t> b;

    void raw(std::initializer_list<uint8_t> by) { b.insert(b.end(), by); }
    void raw(const void *p, size_t n)
    {
        const uint8_t *q = (const uint8_t *)p;
        b.insert(b.end(), q, q + n);
    }
    void text(const char *s) { raw(s, strlen(s) + 1); }

    /* The address the next instruction will be assembled at. */
    uint16_t here() const { return (uint16_t)(TB_ORG + b.size()); }

    void abs(uint8_t op, uint16_t a)
    {
        raw({op, (uint8_t)a, (uint8_t)(a >> 8)});
    }

    void lda(uint8_t v) { raw({0xA9, v}); }
    void ldx(uint8_t v) { raw({0xA2, v}); }
    void ldy(uint8_t v) { raw({0xA0, v}); }
    void lda_abs(uint16_t a) { abs(0xAD, a); }
    void ldx_abs(uint16_t a) { abs(0xAE, a); }
    void sta(uint16_t a) { abs(0x8D, a); }
    void stx(uint16_t a) { abs(0x8E, a); }
    void inc(uint16_t a) { abs(0xEE, a); }
    void jsr(uint16_t a) { abs(0x20, a); }
    void jmp(uint16_t a) { abs(0x4C, a); }
    void bit(uint16_t a) { abs(0x2C, a); }
    void inx() { raw({0xE8}); }
    void dex() { raw({0xCA}); }
    void rts() { raw({0x60}); }
    void stp() { raw({0xDB}); }

    void store(uint16_t a, uint8_t v)
    {
        lda(v);
        sta(a);
    }

    /* The xstack grows down, so the last byte pushed is the first the
     * API pops — a word goes high byte first, a string backwards. */
    void push(uint8_t v) { store(TB_XSTACK, v); }
    void pushw(uint16_t w)
    {
        push((uint8_t)(w >> 8));
        push((uint8_t)w);
    }
    void pushl(uint32_t v)
    {
        push((uint8_t)(v >> 24));
        push((uint8_t)(v >> 16));
        push((uint8_t)(v >> 8));
        push((uint8_t)v);
    }
    void push_str(const char *s)
    {
        size_t n = strlen(s);
        push(0);
        while (n--)
            push((uint8_t)s[n]);
    }

    /* An API call: the op, then the trampoline the 6502 spins in. */
    void call(uint8_t op)
    {
        store(TB_API_OP, op);
        jsr(TB_API_CALL);
    }
    /* With the attribute id, which every op that takes one puts in A. */
    void call_a(uint8_t op, uint8_t a)
    {
        store(TB_API_A, a);
        call(op);
    }

    /* The console, unguarded: the return registers of a call that just
     * finished are already in A and X, and nothing else is competing
     * for the ring. */
    void put_a() { sta(TB_RIA_TX); }
    void put_x() { stx(TB_RIA_TX); }
    void put_ax()
    {
        put_a();
        put_x();
    }
    void put_errno()
    {
        lda_abs(TB_API_ERRNO);
        put_a();
        lda_abs(TB_API_ERRNO + 1);
        put_a();
    }

    /* The console, under the ready bit — what a program printing more
     * than a few bytes has to do. */
    void putc_a()
    {
        raw({0x48}); /* pha */
        bit(TB_RIA_READY);
        raw({0x10, 0xFB}); /* bpl -5 */
        raw({0x68});       /* pla */
        sta(TB_RIA_TX);
    }

    /* One XRAM byte through RW0, which is the port a device snoops. */
    void poke(uint16_t addr, uint8_t val)
    {
        store(TB_RW0_ADDR, (uint8_t)addr);
        store(TB_RW0_ADDR + 1, (uint8_t)(addr >> 8));
        store(TB_RW0_DATA, val);
    }

    /* op 0x01, the xreg dispatch: device, channel, address, one word. */
    void xreg(uint8_t dev, uint8_t ch, uint8_t addr, uint16_t word)
    {
        push(dev);
        push(ch);
        push(addr);
        pushw(word);
        call(0x01);
    }
};

#endif /* _TESTS_BENCH_TB_ASM_H_ */
