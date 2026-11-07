/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "opl_script.h"
#include "opl_lle.h"
#include "core/sys/ring.h"

#include <stdlib.h>
#include <string.h>

void opl_script_free(opl_script_t *s)
{
    free(s->w);
    memset(s, 0, sizeof *s);
}

void opl_script_add(opl_script_t *s, uint32_t n, uint8_t reg, uint8_t val)
{
    if (s->count == s->cap)
    {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->w = realloc(s->w, s->cap * sizeof *s->w);
    }
    s->w[s->count++] = (opl_write_t){n, reg, val};
}

bool opl_is_reg(uint8_t reg)
{
    if (reg == 0x01 || reg == 0x08 || reg == 0xBD)
        return true;
    if (reg >= 0xA0 && reg < 0xD0)
        return (reg & 0x0F) <= 0x0B || (reg & 0x0F) >= 0x0E;
    if ((reg >= 0x20 && reg < 0xA0) || reg >= 0xE0)
        return (reg & 0x1F) < 0x16 && (reg & 0x07) < 6;
    return false;
}

/* The channel registers of channels 0-2, A0-A2, B0-B2 and C0-C2, and AE, AF,
 * BE, BF, CE and CF, which write channels 0-1 the same way. The modulator half
 * of such a write applies in the sample after it. */
static bool opl_late(uint8_t reg)
{
    return reg >= 0xA0 && reg < 0xD0 && ((reg & 0x0F) < 3 || (reg & 0x0F) >= 0x0E);
}

/* Up to 256 writes wait in order. Before each sample, the oldest is applied
 * if the credit is at least 96, which costs 96, unless the sample applies the
 * modulator half of a channel 0-2 write; then the credit gains 72, up to 192
 * while writes wait and 96 while none do. */
uint32_t opl_pace(const opl_script_t *pushes, uint32_t *at, opl_script_t *schedule)
{
    RING(size_t, 256) wait = {0};
    size_t i = 0, k;
    uint32_t drops = 0;
    int credit = 96;
    bool half = false;
    schedule->count = 0;
    schedule->samples = pushes->samples;
    for (uint32_t n = 0; n < pushes->samples; n++)
    {
        for (; i < pushes->count && pushes->w[i].n == n; i++)
        {
            at[i] = OPL_WAITING;
            if (!opl_is_reg(pushes->w[i].reg))
                at[i] = OPL_SKIPPED;
            else if (!RING_PUT(&wait, i))
                at[i] = OPL_DROPPED, drops++;
        }
        if (half)
            half = false;
        else if (credit >= 96 && RING_GET(&wait, &k))
        {
            at[k] = n;
            opl_script_add(schedule, n, pushes->w[k].reg, pushes->w[k].val);
            credit -= 96;
            half = opl_late(pushes->w[k].reg);
        }
        credit += 72;
        const int cap = RING_LEN(&wait) ? 192 : 96;
        if (credit > cap)
            credit = cap;
    }
    for (; i < pushes->count; i++)
        at[i] = OPL_WAITING;
    return drops;
}

/* ---------------- writing scripts ---------------- */

static uint32_t opl_seed;
static opl_script_t *opl_s;
/* The sample the next paced write goes in, and the chance in percent that a
 * paced write is followed by up to 3 more idle samples. */
static uint32_t opl_t;
static int opl_jitter;

static uint32_t opl_rand(void)
{
    uint32_t x = opl_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return opl_seed = x;
}

static int opl_range(int lo, int hi)
{
    return lo + (int)(opl_rand() % (uint32_t)(hi - lo + 1));
}

static bool opl_chance(int percent)
{
    return (int)(opl_rand() % 100) < percent;
}

#define OPL_PICK(...) opl_pick((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))
static uint8_t opl_pick(const uint8_t *v, size_t n)
{
    return v[opl_rand() % n];
}

static const uint8_t opl_op_off[18] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21};

static uint8_t opl_mod(int c)
{
    return opl_op_off[(c / 3) * 6 + c % 3];
}

static uint8_t opl_car(int c)
{
    return (uint8_t)(opl_mod(c) + 3);
}

/* Channel aliases, holes in the map and the timer registers. */
static const uint8_t opl_odd[] = {0xA9, 0xAA, 0xAB, 0xAE, 0xAF, 0xB9, 0xBA, 0xBB, 0xBE, 0xBF,
                                  0xC9, 0xCA, 0xCB, 0xCE, 0xCF, 0x36, 0x37, 0x5E, 0xF6, 0xFF,
                                  0x00, 0x05, 0x10, 0x02, 0x03, 0x04, 0xAC, 0xBC, 0xD0};

/* Every script starts with IC released at sample 0 and test bit 7 held over
 * samples 2-5, which clears the LFO, vibrato and tremolo counters and fills
 * the noise register, wherever the die's power-on left them. The script's
 * own writes start at sample 8. */
static void opl_begin(opl_script_t *s, uint32_t seed, uint32_t samples)
{
    s->count = 0;
    s->samples = samples;
    opl_s = s;
    opl_seed = seed * 2654435761u + 0x9E3779B9u;
    opl_jitter = 0;
    opl_script_add(s, 2, 0x01, 0x80);
    opl_script_add(s, 6, 0x01, 0x00);
    opl_t = 8;
}

/* Two samples apart: applied in the sample it is pushed, and always on the
 * die's bus in time. */
static void opl_w(int reg, int val)
{
    opl_script_add(opl_s, opl_t, (uint8_t)reg, (uint8_t)val);
    opl_t += 2;
    if (opl_jitter && opl_chance(opl_jitter))
        opl_t += (uint32_t)opl_range(0, 3);
}

static void opl_idle(uint32_t n)
{
    opl_t += n;
}

static void opl_idle_to(uint32_t n)
{
    if (opl_t < n)
        opl_t = n;
}

static void opl_op(int off, int r20, int r40, int r60, int r80, int ws)
{
    opl_w(0x20 + off, r20);
    opl_w(0x40 + off, r40);
    opl_w(0x60 + off, r60);
    opl_w(0x80 + off, r80);
    opl_w(0xE0 + off, ws);
}

static void opl_note(int c, int fnum, int block, int key)
{
    opl_w(0xA0 + c, fnum & 0xFF);
    opl_w(0xB0 + c, key << 5 | block << 2 | fnum >> 8);
}

/* ---------------- random, two samples apart ---------------- */

static void opl_random_patch(int off, bool slow)
{
    int ar = slow ? opl_range(4, 11) : OPL_PICK(15, 15, 14, 13, 12, 10, 8, 6, 4);
    int dr = slow ? opl_range(0, 9) : opl_range(0, 15);
    int rr = slow ? opl_range(1, 9) : opl_range(0, 15);
    opl_op(off, opl_range(0, 255), (opl_range(0, 255) & 0xC0) | OPL_PICK(0, 0, 4, 10, 20, 63),
           ar << 4 | dr, opl_range(0, 15) << 4 | rr, opl_range(0, 3));
}

void opl_script_random(opl_script_t *s, uint32_t seed, uint32_t samples, unsigned flags)
{
    opl_begin(s, seed, samples);
    opl_jitter = 30;
    bool slow = flags & OPL_SLOW_ENV;
    opl_w(0x01, opl_chance(80) ? 0x20 : 0);
    opl_w(0x08, OPL_PICK(0, 0x40));
    int bd = opl_chance(50) ? 0xC0 : 0;
    opl_w(0xBD, bd);
    for (int c = 0; c < 9; c++)
    {
        opl_random_patch(opl_mod(c), slow);
        opl_random_patch(opl_car(c), slow);
        opl_w(0xC0 + c, opl_range(0, 15));
        opl_w(0xA0 + c, opl_range(0, 255));
        opl_w(0xB0 + c, opl_range(0, 31));
    }
    int kon[9] = {0};
    while (opl_t < samples - 20)
    {
        int r = opl_range(0, 99), c = opl_range(0, 8);
        int off = opl_chance(50) ? opl_mod(c) : opl_car(c);
        if (r < 25 || r >= 88)
        {
            kon[c] ^= 1;
            opl_w(0xB0 + c, opl_range(0, 31) | kon[c] << 5);
        }
        else if (r < 35)
            opl_w(0xA0 + c, opl_range(0, 255));
        else if (r < 45)
            opl_w(0x40 + off, opl_range(0, 255));
        else if (r < 52)
            opl_w(0x60 + off, opl_range(0, 255));
        else if (r < 58)
            opl_w(0x80 + off, opl_range(0, 255));
        else if (r < 63)
            opl_w(0x20 + off, opl_range(0, 255));
        else if (r < 68)
            opl_w(0xE0 + off, opl_range(0, 3));
        else if (r < 73)
            opl_w(0xC0 + c, opl_range(0, 15));
        else if (r < 80)
        {
            if (!(flags & OPL_RHYTHM))
                continue;
            bd = (bd & 0xE0) | opl_range(0, 31);
            if (opl_chance(15))
                bd ^= 0x20;
            if (opl_chance(10))
                bd ^= 0xC0;
            opl_w(0xBD, bd);
        }
        else if (r < 82)
        {
            bd = (bd & 0x3F) | OPL_PICK(0, 0x40, 0x80, 0xC0);
            opl_w(0xBD, bd);
        }
        else if (r < 84)
            opl_w(0x08, OPL_PICK(0, 0x40, 0x80, 0xC0));
        else if (r < 86)
            opl_w(0x01, OPL_PICK(0x20, 0x20, 0x00) | ((flags & OPL_TEST_BITS) ? OPL_PICK(0, 0, 0x08, 0x80, 0x88) : 0));
        else if (flags & OPL_ALIAS)
            opl_w(opl_odd[opl_rand() % sizeof opl_odd], opl_range(0, 255));
    }
}

/* ---------------- dense: each write at the first sample the bus allows ---------------- */

/* The last dense write as applied, the credit left after it, and the first
 * sample since test bit 7 was last set, where the LFO counts from 0. */
static opl_write_t opl_last;
static int opl_credit;
static uint32_t opl_lfo0;

/* Pushes the write so that it is applied at the first sample, at least
 * 1 + gap after the last one, that the pacing reaches and the bus allows. A
 * write that can follow at once is pushed as the last one is applied, so it
 * waits behind it. The sample after a channel 0-2 write applies none. */
static void opl_dense(int reg, int val, uint32_t gap)
{
    for (uint32_t p = opl_last.n;; p++)
    {
        int credit;
        uint32_t a;
        if (p == opl_last.n)
        {
            credit = opl_credit + 72;
            a = opl_last.n + 1;
        }
        else
        {
            credit = opl_credit + 72 * (int)(p - opl_last.n);
            if (credit > 96)
                credit = 96;
            a = p;
        }
        if (a == opl_last.n + 1 && opl_late(opl_last.reg))
        {
            credit = credit + 72 > 192 ? 192 : credit + 72;
            a++;
        }
        for (; credit < 96; credit += 72)
            a++;
        opl_write_t w = {a, (uint8_t)reg, (uint8_t)val};
        if (a >= opl_last.n + 1 + gap && lle_takes(&opl_last, &w))
        {
            opl_script_add(opl_s, p, w.reg, w.val);
            opl_last = w;
            opl_credit = credit - 96;
            return;
        }
    }
}

/* opl_begin for a script whose writes go through opl_dense. */
static void opl_dense_begin(opl_script_t *s, uint32_t seed, uint32_t samples)
{
    opl_begin(s, seed, samples);
    opl_last = s->w[1];
    opl_credit = 0;
    opl_lfo0 = opl_last.n;
}

void opl_script_dense(opl_script_t *s, uint32_t seed, uint32_t samples, unsigned flags)
{
    opl_dense_begin(s, seed, samples);
    bool slow = flags & OPL_SLOW_ENV, rhythm = flags & OPL_RHYTHM, test = flags & OPL_TEST_BITS;
    uint32_t gap = (flags & OPL_GAP2) ? 1 : 0;
#define D(reg, val) opl_dense(reg, val, gap)
#define DG(reg, val, extra) opl_dense(reg, val, gap + (extra))
#define R20() (opl_range(0, 255) | (opl_chance(50) ? 0x10 : 0))
#define R60()                                                                                   \
    (slow ? opl_range(3, 9) << 4 | opl_range(1, 8)                                              \
          : OPL_PICK(15, 14, 13, 12, 11, 1, 0, 4, 8) << 4 | OPL_PICK(0, 15, 14, 1, 4, 8, 12))
#define R80()                                                                                   \
    (slow ? opl_range(0, 15) << 4 | opl_range(1, 8)                                             \
          : OPL_PICK(0, 15, 14, 1, 7) << 4 | OPL_PICK(0, 15, 14, 1, 4, 8, 12))
#define R40() (OPL_PICK(0, 0x40, 0x80, 0xC0) | OPL_PICK(0, 0, 1, 8, 32, 63))
#define RB0(k) ((k) << 5 | OPL_PICK(0, 7, 6, 1) << 2 | opl_range(0, 3))
    D(0x01, 0x20);
    D(0x08, OPL_PICK(0, 0x40));
    int bd = OPL_PICK(0, 0xC0, 0x40, 0x80);
    D(0xBD, bd);
    for (int c = 0; c < 9; c++)
    {
        for (int i = 0; i < 2; i++)
        {
            int off = i ? opl_car(c) : opl_mod(c);
            D(0x20 + off, R20());
            D(0x40 + off, R40());
            D(0x60 + off, R60());
            D(0x80 + off, R80());
            D(0xE0 + off, opl_range(0, 3));
        }
        D(0xC0 + c, opl_range(0, 15));
        D(0xA0 + c, opl_range(0, 255));
        D(0xB0 + c, RB0(0));
    }
    int kon[9] = {0};
    while (opl_last.n < samples - 60)
    {
        int r = opl_range(0, 99), c = opl_range(0, 8);
        int off = opl_chance(50) ? opl_mod(c) : opl_car(c);
        if (r < 30)
        {
            /* Pairs one sample apart where the bus allows it. */
            switch (opl_range(0, test ? 11 : 9))
            {
            case 0:
                D(0x60 + off, R60());
                kon[c] ^= 1;
                D(0xB0 + c, RB0(kon[c]));
                break;
            case 1:
                D(0x80 + off, R80());
                kon[c] ^= 1;
                D(0xB0 + c, RB0(kon[c]));
                break;
            case 2:
                D(0x20 + off, R20());
                D(0xB0 + c, RB0(kon[c]));
                break;
            case 3:
                kon[c] ^= 1;
                D(0xB0 + c, RB0(kon[c]));
                D(0x08, OPL_PICK(0, 0x40));
                break;
            case 4:
                kon[c] ^= 1;
                D(0xB0 + c, RB0(kon[c]));
                bd = rhythm ? (bd & 0xC0) | opl_range(0, 1) << 5 | opl_range(0, 31)
                            : (bd & 0x1F) | opl_range(0, 3) << 6;
                D(0xBD, bd);
                break;
            case 5:
                D(0x40 + off, R40());
                bd ^= (rhythm && opl_chance(50) ? 0x20 : 0) ^ (rhythm ? opl_range(0, 31) : 0) ^ opl_range(0, 3) << 6;
                D(0xBD, bd);
                break;
            case 6:
                kon[c] = 0;
                D(0xB0 + c, RB0(0));
                kon[c] = 1;
                D(0xB0 + c, RB0(1));
                break;
            case 7:
                D(0xC0 + c, opl_range(0, 15));
                bd ^= opl_range(0, 3) << 6;
                D(0xBD, bd);
                break;
            case 8:
                D(0xA0 + c, opl_range(0, 255));
                D(0x08, OPL_PICK(0, 0x40));
                break;
            case 10:
            {
                /* Test bits for one sample; half the bit 7 pulses where the
                 * LFO is at 63 mod 64, the samples that set the tremolo step. */
                const int t = OPL_PICK(0xA0, 0xA8, 0x28);
                uint32_t g = 0;
                if ((t & 0x80) && opl_chance(50))
                    g = (opl_lfo0 - opl_last.n - 1 + 63) & 63;
                DG(0x01, t, g);
                D(0x01, 0x20);
                if (t & 0x80)
                    opl_lfo0 = opl_last.n;
                break;
            }
            case 11:
                /* Rhythm on or off for one sample. */
                bd ^= 0x20;
                D(0xBD, bd);
                bd ^= 0x20;
                D(0xBD, bd);
                break;
            default:
                kon[c] = 1;
                D(0xB0 + c, RB0(1));
                D(0x60 + off, R60());
                DG(0x60 + off, R60(), (uint32_t)opl_range(0, 40));
                break;
            }
            continue;
        }
        if (r < 60)
        {
            kon[c] ^= 1;
            DG(0xB0 + c, RB0(kon[c]), slow && opl_chance(25) ? (uint32_t)opl_range(0, 300) : 0);
        }
        else if (r < 68)
            D(0xA0 + c, opl_range(0, 255));
        else if (r < 76)
            D(0x40 + off, R40());
        else if (r < 83)
            D(0x60 + off, R60());
        else if (r < 89)
            D(0x80 + off, R80());
        else if (r < 95)
            D(0x20 + off, R20());
        else if (r < 99)
            D(0xE0 + off, opl_range(0, 3));
        else
            D(0xC0 + c, opl_range(0, 15));
        if (rhythm && opl_chance(7))
        {
            bd = (bd & 0xE0) | opl_range(0, 31);
            if (opl_chance(20))
                bd ^= 0x20;
            if (opl_chance(10))
                bd ^= 0xC0;
            D(0xBD, bd);
        }
        if (opl_chance(2))
            D(0x08, OPL_PICK(0, 0x40));
        if ((flags & OPL_ALIAS) && opl_chance(3))
            D(opl_odd[opl_rand() % 15], opl_range(0, 255));
    }
#undef D
#undef DG
#undef R20
#undef R60
#undef R80
#undef R40
#undef RB0
}

/* ---------------- holes on the bus ---------------- */

void opl_script_holes(opl_script_t *s, const opl_script_t *src, uint32_t seed)
{
    uint8_t holes[256];
    size_t nh = 0, k = 0;
    for (int r = 0; r < 256; r++)
        if (!opl_is_reg((uint8_t)r))
            holes[nh++] = (uint8_t)r;
    opl_seed = seed * 2654435761u + 0x9E3779B9u;
    s->count = 0;
    s->samples = src->samples;
    for (size_t i = 0; i < src->count; i++)
    {
        opl_write_t w = src->w[i];
        if (w.reg == 0x08)
            w.val &= 0x7F;
        if (i)
        {
            opl_write_t last = s->w[s->count - 1];
            for (uint32_t m = last.n; m <= w.n; m++)
            {
                const opl_write_t h = {m, holes[k % nh], (uint8_t)opl_rand()};
                if (lle_takes(&last, &h) && lle_takes(&h, &w))
                {
                    opl_script_add(s, h.n, h.reg, h.val);
                    last = h;
                    k++;
                }
            }
        }
        opl_script_add(s, w.n, w.reg, w.val);
    }
}

/* ---------------- directed, two samples apart ---------------- */

void opl_script_power_on(opl_script_t *s)
{
    static const uint8_t ar[9] = {15, 14, 13, 12, 10, 8, 6, 4, 1};
    opl_begin(s, 1, 5000);
    opl_w(0x01, 0x20);
    for (int c = 0; c < 9; c++)
    {
        opl_op(opl_mod(c), 0x21 | (c == 2 ? 0xC0 : 0), 0x18, ar[c] << 4 | 4, 0x35, 0);
        opl_op(opl_car(c), 0x21 | (c == 3 ? 0x80 : 0), 0x00, ar[c] << 4 | 2, 0x26, c & 3);
        opl_w(0xC0 + c, (c & 7) << 1 | (c & 1));
        opl_note(c, 0x100 + 37 * c, 2 + c % 5, 1);
    }
    opl_idle(1500);
    for (int c = 0; c < 9; c++)
        opl_w(0xB0 + c, 0x08 + c % 4);
}

/* Every rate from 0 to 15 in every state, with and without KSR, at blocks 0,
 * 1 and 7; EGT; SL 15; key-off in attack and in decay; AR 15; AR raised to
 * 15 in an attack; NTS. */
void opl_script_envelopes(opl_script_t *s)
{
    static const uint8_t pat[9][4] = {
        {0xF4, 0x26, 0x21, 0}, {0xCA, 0xF3, 0x01, 1}, {0xB8, 0x58, 0x31, 7}, {0x96, 0x75, 0x21, 1},
        {0x43, 0x22, 0x01, 4}, {0x53, 0x12, 0x21, 3}, {0x71, 0x44, 0x31, 5}, {0x66, 0x66, 0x11, 6},
        {0xDF, 0x0F, 0x21, 2}};
    opl_begin(s, 2, 16000);
    opl_w(0x01, 0x20);
    for (int c = 0; c < 9; c++)
    {
        opl_op(opl_mod(c), pat[c][2], 0x20, pat[c][0], pat[c][1], 0);
        opl_op(opl_car(c), pat[c][2] ^ 0x10, 0x00, pat[c][0] ^ 0x11, pat[c][1] ^ 0x10, 0);
        opl_w(0xC0 + c, c & 1);
    }
    for (int c = 0; c < 9; c++)
        opl_note(c, 0x2A0 + 13 * c, pat[c][3], 1);
    opl_idle(300);
    opl_w(0xB0 + 4, 0x10);
    opl_w(0x60 + opl_car(6), 0xF1);
    opl_idle(900);
    opl_w(0xB0 + 5, 0x0C);
    opl_w(0x08, 0x40);
    opl_idle(2000);
    for (int c = 0; c < 9; c++)
        opl_w(0xB0 + c, pat[c][3] << 2 | 2);
    opl_idle(2500);
    opl_w(0x08, 0x00);
    for (int c = 0; c < 9; c++)
    {
        opl_w(0x60 + opl_car(c), (15 - c) << 4 | c);
        opl_w(0x80 + opl_car(c), (c == 8 ? 0xF0 : c << 4) | (15 - c));
        opl_note(c, 0x155 + 61 * c, 7 - c % 8, 1);
    }
    opl_idle(4000);
    for (int c = 0; c < 9; c++)
        opl_w(0xB0 + c, 0x15);
}

/* TL stepped 0 to 63 on held notes, KSL 0-3 over a grid of F-numbers and
 * blocks that reaches the KSL ROM's top. */
void opl_script_levels(opl_script_t *s)
{
    opl_begin(s, 3, 6000);
    opl_w(0x01, 0x20);
    for (int c = 0; c < 9; c++)
    {
        opl_op(opl_mod(c), 0x01, (c & 3) << 6 | 0x14, 0xF2, 0x14, 0);
        opl_op(opl_car(c), 0x02, ((c + 1) & 3) << 6, 0xF2, 0x14, 0);
        opl_w(0xC0 + c, c < 5 ? 0x01 : 0x06);
        opl_note(c, 0x3C0 + 7 * c - (c & 1) * 0x200, c % 8, 1);
    }
    for (int tl = 0; tl < 64; tl++)
    {
        opl_w(0x40 + opl_car(0), tl);
        opl_w(0x40 + opl_mod(5), 0x80 | (63 - tl));
        opl_idle(60);
    }
}

/* WS 0-3 on modulators and carriers; an E0 write while WSE is clear is
 * lost; clearing WSE keeps the stored waveform. */
void opl_script_waves(opl_script_t *s)
{
    opl_begin(s, 4, 6000);
    opl_w(0x01, 0x20);
    for (int c = 0; c < 8; c++)
    {
        opl_op(opl_mod(c), 0x01 + c % 3, 0x18 + c, 0xF3, 0x13, c & 3);
        opl_op(opl_car(c), 0x01, 0x00, 0xF3, 0x13, (c >> 1) & 3);
        opl_w(0xC0 + c, c >= 4 ? 0x0B : 0x04);
        opl_note(c, 0x200 + 29 * c, 3 + c % 3, 1);
    }
    opl_idle(1000);
    opl_w(0x01, 0x00);
    for (int c = 0; c < 8; c++)
        opl_w(0xE0 + opl_car(c), 3 - (c & 3));
    opl_idle(1000);
    opl_w(0x01, 0x20);
    opl_idle(800);
    for (int c = 0; c < 8; c++)
        opl_w(0xE0 + opl_mod(c), (c + 1) & 3);
    opl_w(0x01, 0x00);
    opl_idle(1200);
}

/* FB 0-7, CON 0 and 1, every MULT, block 0 with an odd F-number. */
void opl_script_feedback(opl_script_t *s)
{
    opl_begin(s, 5, 6000);
    opl_w(0x01, 0x20);
    for (int c = 0; c < 9; c++)
    {
        opl_op(opl_mod(c), (c * 5) & 15, 0x00 + c, 0xF2, 0x24, c == 8);
        opl_op(opl_car(c), (11 + c) & 15, 0x04, 0xF2, 0x24, 0);
        opl_w(0xC0 + c, (c & 7) << 1 | (c < 4));
        opl_note(c, c == 0 ? 0x155 : 0x120 + 41 * c, c == 0 ? 0 : 1 + c % 6, 1);
    }
    opl_idle(1600);
    for (int c = 0; c < 9; c++)
        opl_w(0xC0 + c, (7 - (c & 7)) << 1 | (c >= 4));
    opl_idle(1600);
    for (int c = 0; c < 9; c++)
        opl_w(0x20 + opl_car(c), 15 - c);
    opl_idle(1500);
}

/* AM and VIB on and off per slot, DAM and DVB toggled, a whole tremolo
 * period and more than a vibrato period. */
void opl_script_lfo(opl_script_t *s)
{
    opl_begin(s, 6, 16000);
    opl_w(0x01, 0x20);
    opl_w(0xBD, 0xC0);
    for (int c = 0; c < 9; c++)
    {
        opl_op(opl_mod(c), (c & 1 ? 0x80 : 0) | (c & 2 ? 0x40 : 0) | 0x21, 0x1C, 0xF0, 0x05, 0);
        opl_op(opl_car(c), (c & 4 ? 0x80 : 0) | (c % 3 == 0 ? 0x40 : 0) | 0x21, 0x00, 0xF0, 0x05, 0);
        opl_w(0xC0 + c, c & 1);
        opl_note(c, 0x3FF - 37 * c, 4 + c % 3, 1);
    }
    opl_idle_to(4000);
    opl_w(0xBD, 0x80);
    opl_idle_to(8000);
    opl_w(0xBD, 0x40);
    opl_w(0x20 + opl_car(4), 0x61);
    opl_idle_to(11000);
    opl_w(0xBD, 0x00);
    opl_w(0x20 + opl_car(4), 0x21);
    opl_idle_to(14000);
    opl_w(0xBD, 0xC0);
}

/* The drums alone and together, keyed by BD bits and by B6-B8, BD with CON
 * 1, AM on drums, rhythm turned off and on while ch 6-8 sound, re-hits two
 * samples apart. */
void opl_script_rhythm(opl_script_t *s)
{
    opl_begin(s, 7, 9000);
    opl_w(0x01, 0x20);
    opl_op(opl_mod(6), 0x01, 0x0A, 0xF8, 0x46, 0);
    opl_op(opl_car(6), 0x01, 0x00, 0xF6, 0x45, 0);
    opl_op(opl_mod(7), 0x81, 0x00, 0xF7, 0x37, 0);
    opl_op(opl_car(7), 0x01, 0x00, 0xF8, 0x58, 0);
    opl_op(opl_mod(8), 0x05, 0x00, 0xF5, 0x46, 0);
    opl_op(opl_car(8), 0x81, 0x00, 0xF9, 0x68, 0);
    opl_w(0xC0 + 6, 0x08);
    opl_w(0xC0 + 7, 0x0E);
    opl_w(0xC0 + 8, 0x07);
    opl_note(6, 0x157, 2, 0);
    opl_note(7, 0x1C1, 3, 0);
    opl_note(8, 0x2A5, 4, 0);
    for (int d = 0; d < 5; d++)
    {
        opl_w(0xBD, 0x20 | 1 << d);
        opl_idle(400);
        opl_w(0xBD, 0x20);
        opl_idle(100);
    }
    opl_w(0xBD, 0x3F);
    opl_idle(600);
    opl_w(0xBD, 0x20);
    opl_w(0xBD, 0x3F);
    opl_idle(300);
    opl_w(0xC0 + 6, 0x01);
    opl_w(0xBD, 0x30);
    opl_w(0xBD, 0x20);
    opl_w(0xBD, 0x30);
    opl_idle(500);
    opl_w(0xB0 + 7, 0x2D);
    opl_w(0xBD, 0x21);
    opl_idle(500);
    opl_w(0xBD, 0xF5);
    opl_idle(500);
    opl_w(0xBD, 0xC0);
    opl_w(0xB0 + 6, 0x29);
    opl_w(0xB0 + 8, 0x31);
    opl_idle(400);
    opl_w(0xBD, 0xE0);
    opl_w(0xBD, 0xEA);
    opl_idle(400);
    opl_w(0xBD, 0x0A);
    opl_idle(300);
    opl_w(0xBD, 0x3F);
    opl_idle(600);
}

/* Key-off then key-on two samples apart, key-on while keyed, key-off in an
 * attack, glides and block changes every two samples, with vibrato. */
void opl_script_retrigger(opl_script_t *s)
{
    opl_begin(s, 8, 7000);
    opl_w(0x01, 0x20);
    opl_w(0xBD, 0x40);
    for (int c = 0; c < 9; c++)
    {
        opl_op(opl_mod(c), 0x41 | (c & 1) << 4, 0x1A, 0xC3 + c, 0x34, 0);
        opl_op(opl_car(c), 0x41, 0x00, 0x83 + 16 * (c & 3), 0x35, 0);
        opl_w(0xC0 + c, 0x06);
    }
    for (int c = 0; c < 9; c++)
        opl_note(c, 0x180 + 31 * c, 3, 1);
    opl_idle(400);
    for (int c = 0; c < 9; c++)
    {
        opl_w(0xB0 + c, 0x0D);
        opl_w(0xB0 + c, 0x2D);
    }
    opl_idle(300);
    for (int c = 0; c < 9; c++)
        opl_w(0xB0 + c, 0x2E);
    opl_idle(300);
    for (int c = 0; c < 9; c++)
    {
        opl_w(0x60 + opl_car(c), 0x33);
        opl_w(0xB0 + c, 0x0E);
        opl_w(0xB0 + c, 0x2E);
    }
    opl_idle(20);
    for (int c = 0; c < 9; c++)
        opl_w(0xB0 + c, 0x0E);
    opl_idle(400);
    for (int c = 0; c < 3; c++)
        opl_note(c, 0x200, 4, 1);
    for (int i = 0; i < 600; i++)
    {
        int c = i % 3;
        if (i % 40 == 39)
            opl_w(0xB0 + c, 0x20 | ((i / 40) % 8) << 2 | 2);
        else
            opl_w(0xA0 + c, (i * 7 + c * 50) & 0xFF);
    }
    opl_idle(400);
}

/* 18 slots summed in phase, so the mix passes both rails of the DAC; with
 * rhythm, slots 12-17 count twice. */
void opl_script_clip(opl_script_t *s, bool rhythm)
{
    opl_begin(s, 9 + rhythm, 5000);
    opl_w(0x01, 0x20);
    for (int c = 0; c < 9; c++)
    {
        opl_op(opl_mod(c), 0x21, 0x00, 0xF0, 0x0F, rhythm ? c % 3 : 0);
        opl_op(opl_car(c), 0x21, 0x00, 0xF0, 0x0F, rhythm ? 2 - c % 3 : 0);
        opl_w(0xC0 + c, 0x01);
    }
    if (rhythm)
        opl_w(0xBD, 0x3F);
    for (int c = 0; c < 9; c++)
        opl_note(c, 0x080, 2, 1);
    opl_idle(3000);
    for (int c = 0; c < 9; c++)
        opl_w(0x40 + opl_car(c), 0x10);
}

/* Test bits 3, 5 and 7 set and cleared over notes with AM and VIB. Bit 3
 * is often set just before bit 7, so bit 7 lands while the vibrato position
 * is not 0, which slot 0 (VIB on, modulating) shows. */
void opl_script_test_bits(opl_script_t *s, uint32_t seed)
{
    opl_begin(s, 100 + seed, 9000);
    opl_w(0x01, 0x20);
    opl_w(0xBD, OPL_PICK(0x00, 0x40, 0x80, 0xC0, 0xE0));
    for (int c = 0; c < 9; c++)
    {
        opl_op(opl_mod(c), OPL_PICK(0x40, 0xC0, 0x80, 0x00) | opl_range(0, 15), opl_range(0, 40),
               opl_range(10, 15) << 4 | opl_range(0, 6), opl_range(0, 8) << 4 | opl_range(1, 6), opl_range(0, 3));
        opl_op(opl_car(c), OPL_PICK(0x40, 0xC0, 0x80) | opl_range(0, 15), opl_range(0, 10),
               opl_range(10, 15) << 4 | opl_range(0, 6), opl_range(0, 8) << 4 | opl_range(1, 6), opl_range(0, 3));
        opl_w(0xC0 + c, opl_range(0, 15));
        opl_note(c, opl_range(0x180, 0x3FF), opl_range(2, 6), 1);
    }
    opl_op(opl_mod(0), 0x41, 0x08, 0xF0, 0x02, 0);
    opl_w(0xC0, 0x02);
    while (opl_t < s->samples - 300)
    {
        opl_idle((uint32_t)opl_range(20, 500));
        if (opl_chance(40))
        {
            opl_w(0x01, 0x28);
            opl_idle((uint32_t)opl_range(0, 9));
        }
        opl_w(0x01, OPL_PICK(0x20, 0x28, 0xA0, 0xA8, 0x08, 0x80, 0x00, 0x88, 0x20));
        if (opl_chance(30))
        {
            int c = opl_range(0, 8);
            opl_w(0xB0 + c, opl_range(0, 31));
            opl_w(0xB0 + c, 0x20 | opl_range(0, 31));
        }
    }
    opl_w(0x01, 0x20);
}

/* Slow envelopes across the envelope timer's wrap at 2^19 samples. */
void opl_script_eg_wrap(opl_script_t *s)
{
    opl_begin(s, 11, 540000);
    opl_w(0x01, 0x20);
    for (int c = 0; c < 9; c++)
    {
        opl_op(opl_mod(c), 0x01 | (c & 1) << 4, 0x20, (2 + c % 5) << 4 | (1 + c % 4), (c % 6) << 4 | (1 + c % 3), 0);
        opl_op(opl_car(c), 0x21 | (c & 2) << 3, 0x00, (1 + c % 6) << 4 | (1 + c % 5), (c % 4) << 4 | (1 + c % 4), 0);
        opl_w(0xC0 + c, c & 1);
        opl_note(c, 0x120 + 53 * c, 1 + c % 7, 1);
    }
    for (int i = 0; opl_t < s->samples - 5000; i++)
    {
        int c = i % 9;
        opl_idle(opl_t >= 515000 && opl_t < 532000 ? 300 : 4000);
        opl_w(0xB0 + c, (1 + c % 7) << 2 | 1);
        opl_w(0xB0 + c, 0x20 | (1 + c % 7) << 2 | 1);
    }
}

#define D(reg, val) opl_dense(reg, val, 0)
#define DG(reg, val, gap) opl_dense(reg, val, gap)

/* Slots at AR 0 keyed on from silence stay in attack at level 0x1FF, in the
 * mix. Each source keys them off: B0 (its modulator a sample later), the
 * alias BA, B3-B5, B6-B8, BD drum bits and rhythm turned off; a key-on, test
 * bit 7 or NTS follows as soon as the bus allows. */
void opl_script_keyoff_silent(opl_script_t *s)
{
    opl_dense_begin(s, 12, 6000);
    D(0x01, 0x20);
    for (int c = 0; c < 9; c++)
    {
        for (int i = 0; i < 2; i++)
        {
            int off = i ? opl_car(c) : opl_mod(c);
            D(0x20 + off, 0x01 | (c & 1) << 4);
            D(0x60 + off, 0x0F);
            D(0x80 + off, i ? 0x0F : 0x4F);
        }
        D(0xC0 + c, c & 1);
        D(0xA0 + c, 0x55 + 29 * c);
    }
    for (int r = 0; opl_last.n < s->samples - 200; r++)
    {
        const int rhy = r & 1;
        for (int c = 0; c < 9; c++)
            D(0xB0 + c, (c < 6 || !rhy) << 5 | 0x11);
        D(0xBD, rhy ? 0x3F : 0x00);
        DG(0xB0, 0x11, (uint32_t)(r % 4));
        D(0xBA, 0x11);
        D(0xB3 + r % 3, 0x11);
        if (rhy)
        {
            D(0xBD, 0x2E);
            D(0xBD, 0x00);
            D(0xBD, 0x3F);
            D(0xBD, 0x20);
        }
        else
        {
            D(0xB6 + r % 3, 0x11);
            D(0xBD, 0x00);
        }
        D(0x01, OPL_PICK(0x28, 0x20));
        DG(0x01, 0xA0, (uint32_t)opl_range(0, 3));
        D(0x01, 0x20);
        D(0xB4, 0x31);
        D(0xB4, 0x11);
        D(0x08, 0x40);
        D(0x08, 0x00);
        DG(0xBD, 0x00, 20);
    }
}

/* SL 0: attack ends in the SL band, so decay lasts one sample. AR 15 is at
 * level 0 on the key-on; the decay sample takes an SL write or a key-off,
 * and test bit 7 rises while slot 0 is at level 0 in attack. */
void opl_script_decay_sl0(opl_script_t *s)
{
    static const uint8_t ar[9] = {15, 14, 13, 12, 10, 8, 4, 12, 15};
    opl_dense_begin(s, 13, 9000);
    D(0x01, 0x20);
    for (int c = 0; c < 9; c++)
    {
        for (int i = 0; i < 2; i++)
        {
            int off = i ? opl_car(c) : opl_mod(c);
            D(0x20 + off, 0x01 | (c & 1) << 4 | (c & 2) << 4 | i << 6);
            D(0x40 + off, i ? 0x00 : 0x10);
            D(0x60 + off, ar[c] << 4 | (3 * c) % 16);
            D(0x80 + off, 0x01 + c % 7);
            D(0xE0 + off, (c + i) & 3);
        }
        D(0xC0 + c, c & 1);
        D(0xA0 + c, 0x80 + 23 * c);
    }
    for (int r = 0; opl_last.n < s->samples - 600; r++)
    {
        const int rhy = r % 3 == 2;
        D(0xBD, rhy ? 0x20 : 0x00);
        for (int c = 1; c < 9; c++)
            D(0xB0 + c, (c < 6 || !rhy) << 5 | (1 + c % 6) << 2 | 1);
        if (rhy)
            D(0xBD, 0x3F);
        D(0x80 + opl_car(8), 0x20 | (r & 15));
        D(0x80 + opl_car(8), 0x01);
        D(0xB0, 0x25);
        D(0xB0, 0x05);
        D(0x01, 0x28);
        DG(0x01, 0x28, (uint32_t)(r % 5));
        D(0xB9, 0x25);
        D(0x01, 0xA0);
        D(0x01, 0x20);
        DG(0xBD, 0x00, 150 + 37 * (uint32_t)(r % 7));
        for (int c = 0; c < 9; c++)
            D(0xB0 + c, (1 + c % 6) << 2 | 1);
        DG(0x08, (r & 1) << 6, 100);
    }
}

#undef D
#undef DG

/* ---------------- bursts ---------------- */

/* A write pushed before sample n, or a few whose order matters. */
static void opl_burst_write(uint32_t n, unsigned flags, int *bd)
{
    int c = opl_range(0, 8), off = opl_chance(50) ? opl_mod(c) : opl_car(c);
    int r = opl_range(0, 99);
    if (r < 10)
    {
        /* key-off then key-on in one burst */
        opl_script_add(opl_s, n, (uint8_t)(0xB0 + c), (uint8_t)opl_range(0, 31));
        opl_script_add(opl_s, n, (uint8_t)(0xB0 + c), (uint8_t)(0x20 | opl_range(0, 31)));
    }
    else if (r < 15)
    {
        *bd = (*bd & 0xE0) | opl_range(0, 31);
        opl_script_add(opl_s, n, 0xBD, (uint8_t)(*bd & ~0x1F));
        opl_script_add(opl_s, n, 0xBD, (uint8_t)*bd);
    }
    else if (r < 20)
    {
        opl_script_add(opl_s, n, (uint8_t)(0xA0 + c), (uint8_t)opl_range(0, 255));
        opl_script_add(opl_s, n, (uint8_t)(0xB0 + c), (uint8_t)opl_range(0, 63));
        opl_script_add(opl_s, n, (uint8_t)(0xA0 + c), (uint8_t)opl_range(0, 255));
    }
    else if (r < 24)
    {
        opl_script_add(opl_s, n, 0x01, 0x20);
        opl_script_add(opl_s, n, (uint8_t)(0xE0 + off), (uint8_t)opl_range(0, 3));
        opl_script_add(opl_s, n, 0x01, 0x00);
        opl_script_add(opl_s, n, (uint8_t)(0xE0 + off), (uint8_t)opl_range(0, 3));
    }
    else if (r < 28)
    {
        *bd ^= 0x20;
        opl_script_add(opl_s, n, 0xBD, (uint8_t)*bd);
        opl_script_add(opl_s, n, (uint8_t)(0xB6 + opl_range(0, 2)), (uint8_t)opl_range(0, 63));
    }
    else if (r < 33)
    {
        /* a channel register and its alias in turn */
        int a = opl_range(0, 2);
        int g = OPL_PICK(0xA0, 0xB0, 0xC0);
        opl_script_add(opl_s, n, (uint8_t)(g + a), (uint8_t)opl_range(0, 63));
        opl_script_add(opl_s, n, (uint8_t)(g + 9 + a), (uint8_t)opl_range(0, 63));
        opl_script_add(opl_s, n, (uint8_t)(g + a), (uint8_t)opl_range(0, 63));
    }
    else if (r < 38)
        opl_script_add(opl_s, n, opl_odd[opl_rand() % sizeof opl_odd], (uint8_t)opl_range(0, 255));
    else if (r < 40)
        opl_script_add(opl_s, n, 0x01,
                       (uint8_t)(OPL_PICK(0x20, 0x00) | ((flags & OPL_TEST_BITS) ? OPL_PICK(0, 0x08, 0x80, 0x88) : 0)));
    else if (r < 42)
        opl_script_add(opl_s, n, 0x08, OPL_PICK(0, 0x40, 0xC0));
    else if (r < 44)
    {
        /* anywhere on the page but 01, whose bits 0 and 2 the engine leaves out */
        int reg = opl_range(2, 256) & 0xFF;
        opl_script_add(opl_s, n, (uint8_t)reg, (uint8_t)opl_range(0, 255));
    }
    else if (r < 50)
        opl_script_add(opl_s, n, (uint8_t)(0xC0 + c), (uint8_t)opl_range(0, 15));
    else if (r < 55)
        opl_script_add(opl_s, n, (uint8_t)(0xA0 + c), (uint8_t)opl_range(0, 255));
    else if (r < 64)
        opl_script_add(opl_s, n, (uint8_t)(0xB0 + c), (uint8_t)opl_range(0, 63));
    else
        opl_script_add(opl_s, n, (uint8_t)(OPL_PICK(0x20, 0x40, 0x60, 0x80, 0xE0, 0x40, 0x60) + off),
                       (uint8_t)opl_range(0, 255));
}

/* A patch for every voice loaded in one sample, then random bursts of 1-40
 * writes, sometimes a few hundred. Register 01 is written as 0x20 or 0x00
 * unless flags has OPL_TEST_BITS. */
void opl_script_bursts(opl_script_t *s, uint32_t seed, uint32_t samples, unsigned flags)
{
    opl_begin(s, 200 + seed, samples);
    int bd = 0xC0;
    uint32_t n = 8;
    opl_script_add(s, n, 0x01, 0x20);
    for (int c = 0; c < 9; c++)
    {
        for (int i = 0; i < 2; i++)
        {
            int off = i ? opl_car(c) : opl_mod(c);
            opl_script_add(s, n, (uint8_t)(0x20 + off), (uint8_t)opl_range(0, 255));
            opl_script_add(s, n, (uint8_t)(0x40 + off), (uint8_t)(opl_range(0, 255) & 0xC7));
            opl_script_add(s, n, (uint8_t)(0x60 + off), (uint8_t)(opl_range(8, 15) << 4 | opl_range(0, 15)));
            opl_script_add(s, n, (uint8_t)(0x80 + off), (uint8_t)opl_range(0, 255));
            opl_script_add(s, n, (uint8_t)(0xE0 + off), (uint8_t)opl_range(0, 3));
        }
        opl_script_add(s, n, (uint8_t)(0xC0 + c), (uint8_t)opl_range(0, 15));
        opl_script_add(s, n, (uint8_t)(0xA0 + c), (uint8_t)opl_range(0, 255));
        opl_script_add(s, n, (uint8_t)(0xB0 + c), (uint8_t)(0x20 | opl_range(0, 31)));
    }
    opl_script_add(s, n, 0xBD, (uint8_t)bd);
    n += 200;
    while (n < samples - 1000)
    {
        int k = opl_chance(3) ? opl_range(100, 300) : opl_range(1, 40);
        for (int i = 0; i < k; i++)
            opl_burst_write(n, flags, &bd);
        n += (uint32_t)(opl_chance(20) ? opl_range(1, 4) : opl_range(5, 400));
    }
}

/* The opl_init program's preamble: the whole page cleared, then register 01
 * written 2,040 times, then a note, all before one sample; then 40 random
 * writes before every sample for 1,000 samples. */
void opl_script_flood(opl_script_t *s, uint32_t seed)
{
    static const uint8_t note[11][2] = {{0x20, 0x01}, {0x23, 0x01}, {0x40, 0x10}, {0x43, 0x00},
                                        {0x60, 0xF0}, {0x63, 0xF0}, {0x80, 0x77}, {0x83, 0x77},
                                        {0xC0, 0x0E}, {0xA0, 0x98}, {0xB0, 0x31}};
    opl_begin(s, 300 + seed, 6000);
    int bd = 0;
    uint32_t n = 8;
    for (int r = 0xFF; r > 0; r--)
        opl_script_add(s, n, (uint8_t)r, 0);
    for (int i = 0; i < 2040; i++)
        opl_script_add(s, n, 0x01, 0);
    for (int i = 0; i < 11; i++)
        opl_script_add(s, n, note[i][0], note[i][1]);
    n += 1500;
    opl_script_add(s, n, 0x01, 0x20);
    for (int i = 0; i < 11; i++)
        opl_script_add(s, n, note[i][0], note[i][1]);
    for (uint32_t end = n + 1000; n < end; n++)
        for (int i = 0; i < 40; i++)
            opl_burst_write(n, 0, &bd);
}
