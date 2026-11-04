/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Each chip sample runs cycle by cycle: in cycle t slot t is evaluated and
 * slot t - 2 is added to the accumulator, and every register field is read
 * at the cycle the spec gives for it. Writes are applied between samples,
 * with the die's one-sample lags: the modulator side of channels 0-2 and the
 * feedback of channels 3-5 take a channel write one sample later. */

#include "opl_model.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

enum
{
    ATTACK,
    DECAY,
    SUSTAIN,
    RELEASE
};

typedef struct
{
    uint8_t test, nts, rhy, dam, dvb, drums;
} model_glob_t;

static struct
{
    uint8_t mult[18], ksr[18], egt[18], vib[18], am[18], tl[18], ksl[18];
    uint8_t ar[18], dr[18], sl[18], rr[18], ws[18];
    /* What the carriers read. */
    uint16_t fnum[9];
    uint8_t blk[9], kon[9];
    /* What the modulators read, and the CON and FB they use. */
    uint16_t mfnum[9];
    uint8_t mblk[9], mkon[9], mcon[9], mfb[9];
    /* Channel writes the modulator side takes next sample: bit 0 A, 1 B,
     * 2 C, 3 the feedback of C alone. */
    uint8_t late[9], late_val[9][4];
    model_glob_t g;
    uint16_t level[18];
    uint8_t state[18];
    uint32_t phase[18];
    int16_t fb1[9], fb2[9];
    uint8_t consnap[18];
    int16_t val[18];
    uint8_t mute[18];
    int h2, h3, h7, h8, c3, c5;
    uint32_t noise;
    int lfo, vibc, tv, tdir, tof, tstep, tout;
    uint32_t eg_t;
    int eg_carry, eg_sub, eg_shift, eg_low;
    int32_t acc;
    int rhen;
    int egout, dokon;
} M;

static uint16_t model_logsin[256], model_exp[256];

static const uint8_t model_mt[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
static const uint8_t model_kslrom[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
static const uint8_t model_stephi[4][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 0}};
static const uint8_t model_drum_bit[6] = {16, 1, 4, 16, 8, 2};

static int model_ch(int s) { return (s / 6) * 3 + (s % 3); }
static bool model_is_mod(int s) { return (s % 6) < 3; }

/* The slot (0-17) or channel (0-8) a register lands on, or -1; aliases of
 * channels 0-2 give 9-11. */
static int model_target(int a, bool *is_ch)
{
    bool ch = (a & 0xF0) == 0xA0 || (a & 0xF0) == 0xB0 || (a & 0xF0) == 0xC0;
    int add = ch && ((a & 8) || (a & 6) == 6);
    int sel = (a & 1) | ((((a >> 1) + add) & 7) << 1);
    if (!ch)
        sel |= a & 16;
    *is_ch = ch;
    if ((sel & 7) > 5 || sel > 21)
        return -1;
    return sel - 2 * (sel >> 3);
}

static void model_eg(int s, uint16_t fnum, int blk, int kon)
{
    const model_glob_t *g = &M.g;
    int key = kon;
    if (s >= 12 && (s == 12 ? g->rhy : M.rhen) && (g->drums & model_drum_bit[s - 12]))
        key = 1;
    int st = M.state[s], lv = M.level[s];
    int dokon = st == RELEASE && key;
    int rsel = dokon ? ATTACK : st;
    int rate = rsel == ATTACK  ? M.ar[s]
               : rsel == DECAY ? M.dr[s]
               : (rsel == RELEASE || !M.egt[s]) ? M.rr[s]
                                                 : 0;
    int ns = g->nts ? (fnum >> 8 & 1) : (fnum >> 9 & 1);
    int ks = M.ksr[s] ? (blk << 1 | ns) : (blk >> 1);
    int rhi = rate + (ks >> 2);
    if (rhi > 15)
        rhi = 15;
    int rlo = ks & 3;
    int st1 = 0, st2 = 0, st3 = 0;
    if (rhi < 12)
    {
        if (rate && M.eg_sub)
        {
            int sum = rhi + M.eg_shift;
            if (sum == 12 || (sum == 13 && (rlo & 2)) || (sum == 14 && (rlo & 1)))
                st1 = 1;
        }
    }
    else
    {
        int hi = model_stephi[rlo][M.eg_low];
        if (rhi == 12)
            st1 = hi || M.eg_sub;
        else if (rhi == 13)
            st2 = hi, st1 = !hi;
        else if (rhi == 14)
            st3 = hi, st2 = !hi;
        else
            st3 = 1;
    }
    int slv = M.sl[s] == 15 ? 31 : M.sl[s];
    int slreach = (lv >> 4) == slv;
    int zero = lv == 0;
    int silent = (lv & 0x1F8) == 0x1F8;
    int next;
    if (dokon)
        next = ATTACK;
    else if (!key)
        next = RELEASE;
    else if (st == ATTACK)
        next = zero ? DECAY : ATTACK;
    else if (st == DECAY)
        next = slreach ? SUSTAIN : DECAY;
    else
        next = st;
    int linear = !dokon && !silent && (st == SUSTAIN || st == RELEASE || (st == DECAY && !slreach));
    int expo = st == ATTACK && key && rhi != 15 && !zero;
    int mute = st != ATTACK && silent && !dokon;
    int base = mute ? 0x1FF : (dokon && rhi == 15) ? 0 : lv;
    int add = expo ? (~lv) >> 1 : linear ? 4 : 0;
    int inc = st1 ? add >> 2 : st2 ? add >> 1 : st3 ? add : 0;
    int k6 = model_kslrom[fnum >> 6] - 8 * (8 - blk);
    if (k6 < 0)
        k6 = 0;
    int kslv = M.ksl[s] == 0 ? 0 : M.ksl[s] == 1 ? k6 << 1 : M.ksl[s] == 2 ? k6 : k6 << 2;
    int trem = !M.am[s] ? 0 : g->dam ? M.tout >> 2 : M.tout >> 4;
    int tot = lv + kslv + (M.tl[s] << 2) + trem;
    if (tot > 0x1FF)
        tot = 0x1FF;
    if (g->test & 1)
        tot = 0;
    M.egout = tot;
    M.dokon = dokon;
    M.mute[s] = (uint8_t)mute;
    M.level[s] = (uint16_t)((base + inc) & 0x1FF);
    M.state[s] = (uint8_t)next;
}

/* Steps slot s's phase and returns the 10-bit phase it had. */
static int model_pg(int s, uint16_t fnum, int blk)
{
    int f = fnum;
    if (M.vib[s])
    {
        int r = 0;
        if ((M.vibc & 3) == 2)
            r = M.g.dvb ? (fnum >> 7 & 7) : (fnum >> 8 & 3);
        else if (M.vibc & 1)
            r = M.g.dvb ? (fnum >> 8 & 3) : (fnum >> 9 & 1);
        if (M.vibc & 4)
            f = (f - r) & 1023;
        else
            f += r;
    }
    uint32_t inc = ((uint32_t)(((f << blk) >> 1) * model_mt[M.mult[s]])) >> 1;
    int old = M.phase[s] >> 9 & 1023;
    M.phase[s] = (((M.dokon || (M.g.test & 4)) ? 0 : M.phase[s]) + inc) & 0x7FFFF;
    return old;
}

static int16_t model_op(int phase, int ws, int eg)
{
    int sign = phase & 512, quarter = phase & 256;
    int idx = phase & 255;
    if (quarter)
        idx ^= 255;
    int att = model_logsin[idx] + (eg << 3);
    if (att > 4095)
        att = 4095;
    int v = ((model_exp[att & 255] | 1024) << 1) >> (att >> 8);
    if (ws == 1 && sign)
        v = 0;
    if (ws == 3 && quarter)
        v = 0;
    if (ws == 0 && sign)
        v ^= 0x1FFF;
    if (v & 0x1000)
        v -= 0x2000;
    return (int16_t)v;
}

static void model_noise_step(void)
{
    uint32_t n = M.noise;
    uint32_t b = ((n >> 22) ^ (n >> 8)) & 1;
    if (!(n & 0x7FFFFF) || (M.g.test & 0x80))
        b = 1;
    M.noise = ((n << 1) | b) & 0x7FFFFF;
}

static int16_t model_dac(int32_t s)
{
    if (s > 32767)
        s = 32767;
    if (s < -32768)
        s = -32768;
    int top = s >> 9;
    if (top < 0)
        top = ~top;
    top &= 63;
    int sh = top & 32 ? 6 : top & 16 ? 5 : top & 8 ? 4 : top & 4 ? 3 : top & 2 ? 2 : top ? 1 : 0;
    return (int16_t)((s >> sh) * (1 << sh));
}

static void model_eg_timer(void)
{
    int cin = M.eg_carry || M.eg_sub;
    uint32_t t = M.eg_t + (uint32_t)cin;
    M.eg_carry = (int)(t >> 18);
    M.eg_t = t & 0x3FFFF;
    int was = M.eg_sub;
    M.eg_sub ^= 1;
    if (was)
    {
        M.eg_low = M.eg_t & 3;
        M.eg_shift = 0;
        for (int b = 0; b < 13; b++)
            if (M.eg_t >> b & 1)
            {
                M.eg_shift = b + 1;
                break;
            }
    }
}

static int32_t model_run(void)
{
    const model_glob_t *g = &M.g;
    int32_t latch = 0;
    M.tout = M.tv & 127;
    int tcarry = 0;
    for (int t = 0; t < 18; t++)
    {
        /* Tremolo: a 9-bit serial adder, bit t % 9 at cycle t, adding in
         * cycles 0-8. */
        {
            int hold = g->test & 0x80;
            int bi = t % 9;
            int b = (M.tv >> bi) & 1;
            if (t <= 8)
                b += ((M.tstep || (g->test & 8)) && (t == 0 || M.tdir)) + tcarry;
            tcarry = (b >> 1) & 1;
            M.tv = (M.tv & ~(1 << bi)) | ((hold ? 0 : (b & 1)) << bi);
            int of = M.tout == 0 || (M.tout & 105) == 105;
            M.tdir = hold ? 0 : M.tdir ^ (of && !M.tof);
            M.tof = of;
        }
        if (t == 12)
            M.rhen = g->rhy;
        if (model_is_mod(t))
            M.consnap[t] = M.mcon[model_ch(t)];

        int s = t, c = model_ch(s);
        bool mod_slot = model_is_mod(s);
        uint16_t fnum = mod_slot ? M.mfnum[c] : M.fnum[c];
        int blk = mod_slot ? M.mblk[c] : M.blk[c];
        int kon = mod_slot ? M.mkon[c] : M.kon[c];
        model_eg(s, fnum, blk, kon);
        int ph = model_pg(s, fnum, blk);
        if (s == 13)
            M.h2 = ph >> 2 & 1, M.h3 = ph >> 3 & 1, M.h7 = ph >> 7 & 1, M.h8 = ph >> 8 & 1;
        if (s == 17 && g->rhy)
            M.c3 = ph >> 3 & 1, M.c5 = ph >> 5 & 1;
        if (g->rhy && (s == 13 || s == 16 || s == 17))
        {
            int rm = (M.h2 ^ M.h7) | (M.h3 ^ M.c5) | (M.c3 ^ M.c5);
            int nz = (M.noise >> 22) & 1;
            if (s == 13)
                ph = (rm << 9) | ((rm ^ nz) ? 0xD0 : 0x34);
            if (s == 16)
                ph = (M.h8 << 9) | ((M.h8 ^ nz) << 8);
            if (s == 17)
                ph = (rm << 9) | 0x100;
        }
        int mod = 0;
        if (mod_slot)
        {
            if (M.mfb[c] && !(g->rhy && (s == 13 || s == 14)))
                mod = ((M.fb1[c] + M.fb2[c]) >> (9 - M.mfb[c])) & 1023;
        }
        else if (!M.consnap[s - 3] && !(g->rhy && (s == 16 || s == 17)))
            mod = M.val[s - 3] & 1023;
        M.val[s] = model_op((ph + mod) & 1023, M.ws[s], M.egout);

        /* Slot u = t - 2 goes into the accumulator: slots 16 and 17 of the
         * previous sample in cycles 0 and 1. */
        int u = (t + 16) % 18;
        if (t == 17)
        {
            latch = M.acc;
            M.acc = 0;
        }
        int sum = !model_is_mod(u) || M.consnap[u];
        if (g->rhy && (u == 13 || u == 14))
            sum = 1;
        if (g->rhy && u == 12)
            sum = 0;
        if (sum && !M.mute[u])
            M.acc += (g->rhy && u >= 12) ? 2 * M.val[u] : M.val[u];
        if (model_is_mod(u) && t >= 2 && !(g->rhy && (u == 13 || u == 14)))
        {
            int cu = model_ch(u);
            M.fb2[cu] = M.fb1[cu];
            M.fb1[cu] = M.val[u];
        }

        if (g->test & 0x80)
            M.lfo = 0, M.vibc = 0;
        if (t == 17)
        {
            int old = M.lfo;
            if (!(g->test & 0x80))
            {
                M.lfo = (old + 1) & 1023;
                M.vibc = (M.vibc + (old == 1023 || (g->test & 8))) & 7;
            }
            M.tstep = (old & 63) == 63;
            model_eg_timer();
        }
        model_noise_step();
    }
    return latch;
}

static void model_view(int c, int group, uint8_t d)
{
    switch (group)
    {
    case 0:
        M.mfnum[c] = (uint16_t)((M.mfnum[c] & 0x300) | d);
        break;
    case 1:
        M.mfnum[c] = (uint16_t)((M.mfnum[c] & 0xFF) | (d & 3) << 8);
        M.mblk[c] = d >> 2 & 7;
        M.mkon[c] = d >> 5 & 1;
        break;
    case 2:
        M.mcon[c] = d & 1;
        M.mfb[c] = d >> 1 & 7;
        break;
    case 3:
        M.mfb[c] = d >> 1 & 7;
        break;
    }
}

static void model_late(int c, int group, uint8_t d)
{
    M.late[c] |= (uint8_t)(1 << group);
    M.late_val[c][group] = d;
}

void model_write(uint8_t a, uint8_t d)
{
    if (a < 0x20)
    {
        if (a == 0x01)
            M.g.test = d;
        if (a == 0x08)
            M.g.nts = d >> 6 & 1;
        return;
    }
    if (a == 0xBD)
    {
        /* Turning rhythm on latches the cymbal bits from slot 0's phase. */
        if (!M.g.rhy && (d & 0x20))
        {
            int p0 = M.phase[0] >> 9 & 1023;
            M.c3 = p0 >> 3 & 1;
            M.c5 = p0 >> 5 & 1;
        }
        M.g.drums = d & 31;
        M.g.rhy = d >> 5 & 1;
        M.g.dvb = d >> 6 & 1;
        M.g.dam = d >> 7;
        return;
    }
    bool is_ch;
    int lc = model_target(a, &is_ch);
    if (lc < 0)
        return;
    if (!is_ch)
    {
        int s = lc;
        switch (a & 0xE0)
        {
        case 0x20:
            M.mult[s] = d & 15, M.ksr[s] = d >> 4 & 1, M.egt[s] = d >> 5 & 1;
            M.vib[s] = d >> 6 & 1, M.am[s] = d >> 7;
            break;
        case 0x40:
            M.tl[s] = d & 63, M.ksl[s] = d >> 6;
            break;
        case 0x60:
            M.ar[s] = d >> 4, M.dr[s] = d & 15;
            break;
        case 0x80:
            M.sl[s] = d >> 4, M.rr[s] = d & 15;
            break;
        case 0xE0:
            if (M.g.test & 0x20)
                M.ws[s] = d & 3;
            break;
        }
        return;
    }
    int c = lc % 9;
    int group = ((a & 0xF0) - 0xA0) >> 4;
    switch (group)
    {
    case 0:
        M.fnum[c] = (uint16_t)((M.fnum[c] & 0x300) | d);
        break;
    case 1:
        M.fnum[c] = (uint16_t)((M.fnum[c] & 0xFF) | (d & 3) << 8);
        M.blk[c] = d >> 2 & 7;
        M.kon[c] = d >> 5 & 1;
        break;
    }
    if (lc >= 9)
    {
        /* An alias of channels 0-2 lands late in the previous sample, so
         * both slots see it now, and it is the register's newest value. */
        model_view(c, group, d);
        M.late[c] &= (uint8_t)~(1 << group);
    }
    else if (c < 3)
        model_late(c, group, d);
    else if (group == 2 && c < 6)
    {
        M.mcon[c] = d & 1;
        model_late(c, 3, d);
    }
    else
        model_view(c, group, d);
}

int16_t model_sample(void)
{
    int16_t out = model_dac(model_run());
    for (int c = 0; c < 9; c++)
    {
        for (int group = 0; group < 4; group++)
            if (M.late[c] & (1 << group))
                model_view(c, group, M.late_val[c][group]);
        M.late[c] = 0;
    }
    return out;
}

void model_reset(void)
{
    for (int i = 0; i < 256; i++)
    {
        model_logsin[i] = (uint16_t)lround(-log2(sin((i + 0.5) * 3.14159265358979323846 / 512.0)) * 256.0);
        model_exp[i] = (uint16_t)lround((exp2((255 - i) / 256.0) - 1.0) * 1024.0);
    }
    memset(&M, 0, sizeof M);
    for (int s = 0; s < 18; s++)
    {
        M.level[s] = 0x1FF;
        M.state[s] = RELEASE;
    }
    M.tof = 1;
    M.noise = 1;
}
