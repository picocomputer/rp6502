/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Section numbers refer to ym3812.md, the per-sample YM3812 specification.
 *
 * A write is applied before the sample it first affects (spec 7.6). Its
 * effects are cached where they are needed: per slot the phase increment, the
 * KSL + TL term, the rate indices and the waveform; per channel the views the
 * slots read. Each slot keeps an envelope record for its state (rate code,
 * level range); inside the range a sample is one table step, leaving it is a
 * stage change, and span 0 makes the slot re-evaluate the whole rule next. */

#include "core/aud/ym3812.h"
#include <string.h>

/* A test that compiles this file defines this to see each write as it is
 * applied. */
#ifndef YM3812_TRACE
#define YM3812_TRACE(reg, val)
#endif

#pragma GCC push_options
#pragma GCC optimize("O3")

#if defined(_MSC_VER)
#include <intrin.h>
#define YM_HOT static __forceinline
#define YM_COLD static __declspec(noinline)
static inline uint32_t ym_ctz(uint32_t x)
{
    unsigned long i;
    _BitScanForward(&i, x);
    return (uint32_t)i;
}
static inline uint32_t ym_bits(uint32_t x)
{
    unsigned long i;
    _BitScanReverse(&i, x);
    return (uint32_t)i + 1;
}
#else
#define YM_HOT static inline __attribute__((always_inline))
#define YM_COLD static __attribute__((noinline))
#define ym_ctz(x) ((uint32_t)__builtin_ctz(x))
#define ym_bits(x) (32 - (uint32_t)__builtin_clz(x))
#endif

#if defined(__GNUC__) && defined(__arm__)
#define USAT(x, n) ((uint32_t)__builtin_arm_usat((int32_t)(x), (n)))
#define SSAT(x, n) __builtin_arm_ssat((int32_t)(x), (n))
/* A register shift by 32 or more gives 0. */
static inline uint32_t lslr(uint32_t x, uint32_t n)
{
    uint32_t r;
    __asm__("lsl %0, %1, %2" : "=r"(r) : "r"(x), "r"(n));
    return r;
}
#else
static inline uint32_t USAT(int32_t x, int n)
{
    const int32_t m = (1 << n) - 1;
    return (uint32_t)(x < 0 ? 0 : x > m ? m : x);
}
static inline int32_t SSAT(int32_t x, int n)
{
    const int32_t m = (1 << (n - 1)) - 1;
    return x > m ? m : x < -m - 1 ? -m - 1 : x;
}
static inline uint32_t lslr(uint32_t x, uint32_t n)
{
    return n >= 32 ? 0 : x << n;
}
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "a step word holds rate rhi * 4 + n in its byte n");
#endif
_Static_assert(YM3812_QUEUE_SIZE == 256, "the queue head wraps as a uint8_t");

/* log-sin[256], then (exp | 1024) << 1 [256] (spec 4.1) */
static const uint16_t ym_rom[512] = {
#include "ym3812_rom.h"
};

enum
{
    ATT,
    DEC,
    SUS,
    REL
};
#define NONE 64 /* rate index with no step */
#define ATTK 72 /* rate codes from here are the attack form */

/* A slot's view of its channel's F-number, block and KON, and CON, FB. */
typedef struct
{
    uint16_t fnum;
    uint8_t blk, kon, con, fb;
    uint8_t k6; /* KSL attenuation before the KSL shift (spec 3.5) */
} view_t;

typedef struct
{
    uint32_t P;        /* 19-bit phase accumulator << 13 */
    int32_t level;     /* 9-bit attenuation */
    uint16_t lo, span; /* fast path while (level - lo) < span; span 0: re-evaluate */
    uint8_t rc;        /* rate code: step table index, ATTK + rate index in attack */
    uint8_t state;
    uint8_t key;
    uint8_t qm;    /* muted at a fixed point */
    uint32_t ta;   /* KSL + 4 TL | AM mask (0 or 0xff) << 16 */
    uint32_t wav;  /* zero mask (0x100 or 0x200) | 1 << 31 for the NOT half */
    int32_t mute;  /* -1 summed, 0 muted, this sample */
    uint32_t inc;  /* increment << 13 at the view's F-number */
    uint32_t mul;  /* (f & km) * mul, bit 12 cleared, is the increment << 13 at f */
    uint16_t fnum; /* the view's F-number, which vibrato offsets */
    uint16_t km;
    uint32_t r6w; /* rate index per state, byte per state */
    uint8_t slv;  /* SL, 31 for 15 */
    uint8_t nsx;  /* NTS flips bit 0 of the rate indices */
    uint8_t r20, r40, r60, r80, ws;
    const view_t *v; /* the channel view this slot reads (spec 7.6) */
} slot_t;

typedef struct
{
    uint32_t hist; /* modulator's last output << 16 | the one before */
    int32_t incl;  /* -1 when CON sums the modulator */
    uint8_t fbl;   /* 13 + FB, or 32 for FB = 0 */
} chan_t;

typedef struct
{
    uint32_t step[36]; /* bytes [r6]: linear step, [ATTK + r6]: attack multiplier */
    slot_t sl[18];
    chan_t ch[9];
    view_t cv[9], mv[9]; /* carrier view, modulator view (spec 7.6) */
    uint8_t rlw[3];      /* step words set on the last sub = 1 sample */
    uint8_t lag;         /* follows next sample: bits 0-2 ch 0-2 modulator view, 3-5 ch 3-5 FB */
    uint8_t v0fix;       /* test bit 7 rose: slot 0's vibrato position this sample */
    uint8_t test, nts, bd, rhy, dam, dvb;
    uint8_t vib, tdir, tof, tstep, sub, carry, egshift, eglow;
    uint8_t cb; /* c3, c5 at bits 3, 5 */
    uint8_t credit;
    uint16_t lfo, tv;
    uint32_t T, N;
    int32_t held15, held1617;
} ym3812_t;

static ym3812_t ym;
ym3812_queue_t ym3812_queue;

const uint8_t ym3812_regs[32] = {
    0x02, 0x01, 0x00, 0x00, 0x3F, 0x3F, 0x3F, 0x00, /* 01, 08, 20-35 */
    0x3F, 0x3F, 0x3F, 0x00, 0x3F, 0x3F, 0x3F, 0x00, /* 40-55, 60-75 */
    0x3F, 0x3F, 0x3F, 0x00, 0xFF, 0xCF, 0xFF, 0xEF, /* 80-95, A0-AB AE AF, B0-BB BD-BF */
    0xFF, 0xCF, 0x00, 0x00, 0x3F, 0x3F, 0x3F, 0x00, /* C0-CB CE CF, E0-F5 */
};

static const uint8_t MT[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
static const uint8_t KSLROM[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
static const uint8_t KSLMUL[4] = {0, 2, 1, 4};
/* operator offset & 0x1f -> slot; 6, 7, 14 and 15 are not operator offsets */
static const uint8_t OFF2SLOT[22] = {0, 1, 2, 3, 4, 5, 0, 0, 6, 7, 8, 9, 10, 11, 0, 0,
                                     12, 13, 14, 15, 16, 17};
static const uint8_t SLOTCH[18] = {0, 1, 2, 0, 1, 2, 3, 4, 5, 3, 4, 5, 6, 7, 8, 6, 7, 8};
static const uint8_t MSLOT[9] = {0, 1, 2, 6, 7, 8, 12, 13, 14};
static const uint8_t DRUMBIT[6] = {16, 1, 4, 16, 8, 2};
/* vibrato offset fnum >> VSH[DVB][position] (10: none), negative at positions 4-7 */
static const uint8_t VSH[2][8] = {{10, 9, 8, 9, 10, 9, 8, 9}, {10, 8, 7, 8, 10, 8, 7, 8}};
/* attack multipliers (2, 4, 8 for step sizes 1, 2, 4) for rates 48..63 by [sub][eglow];
 * the linear steps are half of these */
static const uint32_t HIROW[2][4][4] = {
    {{0x02020200, 0x04040402, 0x08080804, 0x08080808},
     {0x02000000, 0x04020202, 0x08040404, 0x08080808},
     {0x02020000, 0x04040202, 0x08080404, 0x08080808},
     {0x00000000, 0x02020202, 0x04040404, 0x08080808}},
    {{0x02020202, 0x04040402, 0x08080804, 0x08080808},
     {0x02020202, 0x04020202, 0x08040404, 0x08080808},
     {0x02020202, 0x04040202, 0x08080404, 0x08080808},
     {0x02020202, 0x02020202, 0x04040404, 0x08080808}}};
/* step words for rates below 48 by egshift on sub = 1 samples (17: unused word) */
static const uint8_t RLW[14][3] = {
    {17, 17, 17}, {11, 17, 17}, {10, 11, 17}, {9, 10, 11}, {8, 9, 10}, {7, 8, 9}, {6, 7, 8},
    {5, 6, 7}, {4, 5, 6}, {3, 4, 5}, {2, 3, 4}, {1, 2, 3}, {0, 1, 2}, {17, 0, 1}};
static const uint32_t WAV[4] = {0x80000000u, 0x200, 0, 0x100};

#define STEP(i) (((const uint8_t *)ym.step)[i])

static inline int is_mod(int s) { return (s % 6) < 3; }

static inline uint32_t r6(const slot_t *s, uint32_t st)
{
    return (s->r6w >> (st * 8)) & 0xff;
}

/* the increment << 13 at F-number f (spec 2) */
static inline uint32_t inc_at(const slot_t *s, uint32_t f)
{
    return ((f & s->km) * s->mul) & ~0x1000u;
}

/* this sample's increment: vibrato offset fnum >> vsh, negated when vs is ~0 */
YM_HOT uint32_t slot_inc(const slot_t *s, uint32_t vsh, uint32_t vs)
{
    if (!(s->r20 & 0x40))
        return s->inc;
    const uint32_t f = s->fnum;
    return inc_at(s, f + (((f >> vsh) ^ vs) - vs));
}

/* ---------------- envelope events ---------------- */

static void eg_muted(slot_t *s)
{
    s->level = 0x1ff, s->mute = 0;
    s->lo = 0x1ff, s->span = 1, s->rc = NONE, s->qm = 1;
}

/* Record for the state and level, key consistent with the state. A level
 * at a transition point gets a range that excludes it. */
static void eg_rec(slot_t *s)
{
    const int st = s->state, lv = s->level;
    s->mute = -1, s->qm = 0;
    if (st == ATT)
    {
        const uint32_t a = r6(s, ATT);
        s->lo = 1, s->span = 0x1ff;
        s->rc = (uint8_t)(ATTK + (a >= 60 ? NONE : a));
        return;
    }
    if (lv == 0x1ff && (st != DEC || s->slv != 31))
    {
        eg_muted(s);
        return;
    }
    s->rc = (uint8_t)r6(s, (uint32_t)st);
    int lo = 0, top = 0x1f8;
    if (st == DEC)
    {
        const int band = s->slv << 4;
        if (lv >= band + 16)
            lo = band + 16;
        else
            top = band;
    }
    if (top > lo)
        s->lo = (uint16_t)lo, s->span = (uint16_t)(top - lo);
    else
        s->lo = 0x200, s->span = 1;
}

/* span 0: a write or key change: the whole of spec 3.4 for this sample */
YM_COLD void eg_forced(slot_t *s, uint32_t inc)
{
    if (ym.v0fix && s == ym.sl)
    {
        /* test bit 7 rose: slot 0 still steps at the old vibrato position */
        const uint32_t pos = ym.v0fix;
        const uint32_t d = slot_inc(s, VSH[ym.dvb][pos], 0u - (pos >> 2)) - inc;
        ym.v0fix = 0;
        s->P += d;
        inc += d;
    }
    const int st = s->state, k = s->key, lv = s->level;
    if (st == REL && k)
    {
        /* key-on edge: no step, phase restarts, attack from this level */
        if (r6(s, ATT) - 60 < 4)
            s->level = 0;
        s->state = ATT;
        s->P = inc;
        eg_rec(s);
        return;
    }
    if (st != REL && !k)
    {
        /* key-off: this sample steps with the old state's rate */
        int mute = -1;
        if (st != ATT)
        {
            if (lv >= 0x1f8)
                s->level = 0x1ff, mute = 0;
            else if (st == SUS || (lv >> 4) != s->slv)
                s->level = lv + STEP(r6(s, (uint32_t)st));
        }
        s->state = REL;
        eg_rec(s);
        if (s->mute != mute)
            s->span = 0;
        s->mute = mute;
        return;
    }
    eg_rec(s);
    if ((uint32_t)(lv - s->lo) < s->span)
    {
        const uint32_t rc = s->rc;
        int32_t d = STEP(rc);
        if (rc >= ATTK)
            d = (~lv * d) >> 4;
        s->level = lv + d;
        return;
    }
    s->span = 2; /* the level is at a transition point: take it below */
}

/* The level left its range: attack reached 0, decay reached the SL band, or
 * the level reached 0x1f8. No step this sample (spec 3.4). */
YM_COLD void eg_event(slot_t *s, uint32_t inc)
{
    if (!s->span)
    {
        eg_forced(s, inc);
        if (s->span != 2)
            return;
    }
    const int st = s->state, lv = s->level;
    if (st == ATT)
    {
        s->state = DEC;
        s->lo = 0, s->span = (uint16_t)(s->slv << 4), s->rc = (uint8_t)r6(s, DEC);
        return;
    }
    if (st == DEC && (lv >> 4) == s->slv)
    {
        s->state = SUS;
        if (lv < 0x1f8)
        {
            s->lo = 0, s->span = 0x1f8, s->rc = (uint8_t)r6(s, SUS);
            return;
        }
    }
    eg_muted(s);
}

/* ---------------- per-sample kernels ---------------- */

YM_HOT uint32_t eg(slot_t *s, uint32_t amv, uint32_t inc)
{
    const int32_t e = s->level;
    const uint32_t ta = s->ta;
    const uint32_t eo = USAT(e + (int32_t)(ta & 0xffff) + (int32_t)(amv & (ta >> 16)), 9);
    if ((uint32_t)(e - s->lo) < s->span)
    {
        const uint32_t rc = s->rc;
        int32_t d = STEP(rc);
        if (rc >= ATTK)
            d = (~e * d) >> 4;
        s->level = e + d;
    }
    else if (s->span)
    {
        /* stage change, no step this sample */
        const int st = s->state;
        if (st == ATT)
        {
            s->state = DEC;
            s->lo = 0, s->span = (uint16_t)(s->slv << 4), s->rc = (uint8_t)r6(s, DEC);
        }
        else if (st == DEC && (e >> 4) == s->slv && e < 0x1f8)
        {
            s->state = SUS;
            s->lo = 0, s->span = 0x1f8, s->rc = (uint8_t)r6(s, SUS);
        }
        else
        {
            if (st == DEC && (e >> 4) == s->slv)
                s->state = SUS;
            s->level = 0x1ff, s->mute = 0;
            s->lo = 0x1ff, s->span = 1, s->rc = NONE, s->qm = 1;
        }
    }
    else
        eg_event(s, inc);
    return eo;
}

YM_HOT int32_t op(const slot_t *s, uint32_t p, uint32_t eo)
{
    p &= 0x3ff;
    const uint32_t idx = (p ^ (uint32_t)((int32_t)(p << 23) >> 31)) & 0xff;
    const uint32_t att = USAT((int32_t)(ym_rom[idx] + (eo << 3)), 12);
    int32_t v = (int32_t)(ym_rom[256 + (att & 0xff)] >> (att >> 8));
    const uint32_t w = s->wav;
    if (p & w)
        v = 0;
    return v ^ ((int32_t)(w & (p << 22)) >> 31);
}

/* One melodic channel: returns the modulator's contribution, the carrier's
 * in *car (channels 6-8 hold it for the next output). */
YM_HOT int32_t chan(const int c, uint32_t amv, uint32_t vsh, uint32_t vs, int32_t *car)
{
    slot_t *m = &ym.sl[MSLOT[c]], *k = m + 3;
    chan_t *h = &ym.ch[c];
    const uint32_t pm = m->P, pk = k->P;
    const uint32_t im = slot_inc(m, vsh, vs), ik = slot_inc(k, vsh, vs);
    m->P = pm + im;
    k->P = pk + ik;
    const uint32_t hs = h->hist;
    const int32_t h1 = (int32_t)hs >> 16;
    const uint32_t fb = lslr((uint32_t)(h1 + (int16_t)hs), h->fbl) >> 22;
    if (m->qm & k->qm)
    {
        const int32_t mo = (int32_t)(m->wav & (((pm >> 22) + fb) << 22)) >> 31;
        h->hist = ((uint32_t)h1 & 0xffff) | (uint32_t)mo << 16;
        *car = 0;
        return 0;
    }
    const uint32_t em = eg(m, amv, im);
    const int32_t mo = op(m, (pm >> 22) + fb, em);
    h->hist = ((uint32_t)h1 & 0xffff) | (uint32_t)mo << 16;
    const uint32_t ek = eg(k, amv, ik);
    const int32_t ko = op(k, (pk >> 22) + (uint32_t)(mo & ~h->incl), ek);
    *car = ko & k->mute;
    return mo & m->mute & h->incl;
}

/* Channels 6-8 in rhythm mode (spec 5): returns HH + TOM, holds BD, SD, TC. */
YM_HOT int32_t rhythm(uint32_t amv, uint32_t vsh, uint32_t vs)
{
    slot_t *s = ym.sl;
    int32_t acc;
    {
        slot_t *m = &s[12], *k = &s[15];
        chan_t *h = &ym.ch[6];
        const uint32_t pm = m->P, pk = k->P;
        const uint32_t im = slot_inc(m, vsh, vs), ik = slot_inc(k, vsh, vs);
        m->P = pm + im;
        k->P = pk + ik;
        const uint32_t hs = h->hist;
        const int32_t h1 = (int32_t)hs >> 16;
        const uint32_t fb = lslr((uint32_t)(h1 + (int16_t)hs), h->fbl) >> 22;
        const uint32_t em = eg(m, amv, im);
        const int32_t mo = op(m, (pm >> 22) + fb, em);
        h->hist = ((uint32_t)h1 & 0xffff) | (uint32_t)mo << 16;
        const uint32_t ek = eg(k, amv, ik);
        const int32_t ko = op(k, (pk >> 22) + (uint32_t)(mo & ~h->incl), ek);
        ym.held15 = 2 * (ko & k->mute);
    }
    const uint32_t N = ym.N;
    uint32_t hb;
    {
        slot_t *m = &s[13];
        const uint32_t p = m->P, i = slot_inc(m, vsh, vs);
        m->P = p + i;
        const uint32_t e = eg(m, amv, i);
        hb = p >> 22;
        const uint32_t cb = ym.cb;
        const uint32_t rm = (((hb >> 2) ^ (hb >> 7)) | ((hb >> 3) ^ (cb >> 5)) | ((cb >> 3) ^ (cb >> 5))) & 1;
        const uint32_t hp = (rm << 9) | ((rm ^ (N >> 9)) & 1 ? 0xd0 : 0x34);
        acc = 2 * (op(m, hp, e) & m->mute);
    }
    {
        slot_t *k = &s[16];
        const uint32_t p = k->P, i = slot_inc(k, vsh, vs);
        k->P = p + i;
        const uint32_t e = eg(k, amv, i);
        const uint32_t h8 = (hb >> 8) & 1;
        ym.held1617 = op(k, (h8 << 9) | (((h8 ^ (N >> 6)) & 1) << 8), e) & k->mute;
    }
    {
        slot_t *m = &s[14];
        const uint32_t p = m->P, i = slot_inc(m, vsh, vs);
        m->P = p + i;
        const uint32_t e = eg(m, amv, i);
        acc += 2 * (op(m, p >> 22, e) & m->mute);
    }
    {
        slot_t *k = &s[17];
        const uint32_t p = k->P, i = slot_inc(k, vsh, vs);
        k->P = p + i;
        const uint32_t e = eg(k, amv, i);
        const uint32_t cb = p >> 22;
        ym.cb = (uint8_t)(cb & 0x28);
        const uint32_t rm = (((hb >> 2) ^ (hb >> 7)) | ((hb >> 3) ^ (cb >> 5)) | ((cb >> 3) ^ (cb >> 5))) & 1;
        ym.held1617 += op(k, (rm << 9) | 0x100, e) & k->mute;
    }
    return acc;
}

/* ---------------- write application ---------------- */

static inline void force(slot_t *s)
{
    s->span = 0;
    s->qm = 0;
}

static inline void set_key(slot_t *s, uint32_t k)
{
    if (s->key != k)
    {
        s->key = (uint8_t)k;
        force(s);
    }
}

static void view_k6(view_t *v)
{
    const int k6 = KSLROM[v->fnum >> 6] - 8 * (8 - v->blk);
    v->k6 = (uint8_t)(k6 < 0 ? 0 : k6);
}

/* inc << 13 = ((((f << blk) >> 1) * mt) >> 1) << 13 = ((f & km) * (mt << (11 + blk)))
 * with bit 12 cleared, km clearing bit 0 when blk = 0 (mod 2^32) */
static void slot_pg(slot_t *s)
{
    const view_t *v = s->v;
    const uint32_t bk = v->blk;
    s->fnum = v->fnum;
    s->km = bk ? 0xffff : 0xfffe;
    s->mul = (uint32_t)MT[s->r20 & 15] << (11 + bk);
    s->inc = inc_at(s, s->fnum);
}

static inline void slot_ksl(slot_t *s)
{
    s->ta = (s->ta & 0xffff0000u) | (uint32_t)(s->v->k6 * KSLMUL[s->r40 >> 6] + ((s->r40 & 63) << 2));
}

/* rate index per state, bytewise: 4R + ks, 60 + (ks & 3) above 63, NONE for R = 0 */
static void slot_eg(slot_t *s)
{
    const view_t *v = s->v;
    const uint32_t r20 = s->r20, r60 = s->r60, r80 = s->r80;
    const uint32_t ksr = r20 & 0x10;
    const uint32_t ns = (v->fnum >> (ym.nts ? 8 : 9)) & 1;
    const uint32_t ks = ksr ? (uint32_t)(v->blk << 1) | ns : (uint32_t)v->blk >> 1;
    const uint32_t rr = r80 & 15;
    const uint32_t R = (r60 >> 4) | (r60 & 15) << 8 | ((r20 & 0x20) ? 0 : rr) << 16 | rr << 24;
    uint32_t x = (R << 2) + ks * 0x01010101u;            /* bytes <= 75 */
    const uint32_t ov = ((x >> 6) & 0x01010101u) * 0xff; /* bytes > 63 */
    x = (x & ~ov) | ((60 + (ks & 3)) * 0x01010101u & ov);
    const uint32_t nz = (R | R >> 1 | R >> 2 | R >> 3) & 0x01010101u;
    const uint32_t z = (nz ^ 0x01010101u) * 0xff; /* bytes with R = 0 */
    const uint32_t w = (x & ~z) | (NONE * 0x01010101u & z);
    const uint32_t slv = (r80 >> 4) == 15 ? 31 : r80 >> 4;
    s->nsx = (uint8_t)(ksr && (((v->fnum >> 8) ^ (v->fnum >> 9)) & 1));
    if (w != s->r6w || slv != s->slv)
    {
        s->r6w = w;
        s->slv = (uint8_t)slv;
        if (s->span)
            eg_rec(s);
    }
}

static inline uint32_t drum(int i)
{
    return ym.rhy && (ym.bd & DRUMBIT[i - 12]);
}

/* a slot's view changed: what = 1 F-number low bits, 3 F-number high bits
 * or block, | 4 key; the key settles first so a key change rebuilds once */
static void slot_view(int i, int what)
{
    slot_t *s = &ym.sl[i];
    if (what & 4)
        set_key(s, s->v->kon | (i >= 12 && drum(i)));
    if (what & 1)
    {
        slot_pg(s);
        slot_ksl(s);
        if (what & 2)
            slot_eg(s);
    }
}

static inline void chan_fb(int c)
{
    chan_t *h = &ym.ch[c];
    const view_t *v = &ym.mv[c];
    h->incl = v->con ? -1 : 0;
    h->fbl = (uint8_t)(v->fb ? 13 + v->fb : 32);
}

/* A0: F-number low bits. Returns 1 if they changed. */
static inline int view_a(view_t *v, uint32_t d)
{
    const uint32_t f = (v->fnum & 0x300) | d;
    if (f == v->fnum)
        return 0;
    const uint32_t hi = (f ^ v->fnum) >> 6;
    v->fnum = (uint16_t)f;
    if (hi)
        view_k6(v);
    return 1;
}

/* B0: returns 3 if F-number high bits or block changed, | 4 for the key */
static inline int view_b(view_t *v, uint32_t d)
{
    const uint32_t f = (v->fnum & 0xff) | (d & 3) << 8, b = (d >> 2) & 7, k = (d >> 5) & 1;
    int what = 0;
    if (f != v->fnum || b != v->blk)
    {
        v->fnum = (uint16_t)f, v->blk = (uint8_t)b;
        view_k6(v);
        what = 3;
    }
    if (k != v->kon)
        v->kon = (uint8_t)k, what |= 4;
    return what;
}

/* channel 0-2: the modulator view becomes the carrier view */
static void mv_follow(int c)
{
    view_t *mv = &ym.mv[c];
    const view_t *cv = &ym.cv[c];
    int what = 0;
    if (mv->fnum != cv->fnum || mv->blk != cv->blk)
        what = (mv->blk != cv->blk || ((mv->fnum ^ cv->fnum) >> 8)) ? 3 : 1;
    if (mv->kon != cv->kon)
        what |= 4;
    *mv = *cv;
    if (what)
        slot_view(MSLOT[c], what);
    chan_fb(c);
}

/* A0-A8 / B0-B8: alias 1 for A9-AB / B9-BB (both slots now, spec 7.6) */
static void chan_ab(int c, uint32_t g, uint32_t d, int alias)
{
    const int ms = MSLOT[c];
    view_t *cv = &ym.cv[c];
    const int what = g == 0xa ? view_a(cv, d) : view_b(cv, d);
    if (what)
        slot_view(ms + 3, what);
    if (c < 3)
    {
        if (!alias)
            ym.lag |= (uint8_t)(1 << c);
        else
        {
            view_t *mv = &ym.mv[c];
            const int w2 = g == 0xa ? view_a(mv, d) : view_b(mv, d);
            if (w2)
                slot_view(ms, w2);
        }
        return;
    }
    if (what)
    {
        view_t *mv = &ym.mv[c];
        mv->fnum = cv->fnum, mv->blk = cv->blk, mv->kon = cv->kon, mv->k6 = cv->k6;
        slot_view(ms, what);
    }
}

/* C0-C8: FB and CON; alias 1 for C9-CB */
static void chan_c(int c, uint32_t d, int alias)
{
    view_t *cv = &ym.cv[c], *mv = &ym.mv[c];
    cv->con = d & 1;
    cv->fb = (uint8_t)((d >> 1) & 7);
    if (c < 3 && !alias)
    {
        ym.lag |= (uint8_t)(1 << c);
        return;
    }
    mv->con = cv->con;
    if (c >= 3 && c < 6)
        ym.lag |= (uint8_t)(1 << c);
    else
        mv->fb = cv->fb;
    chan_fb(c);
}

static void bd_write(uint32_t v)
{
    const uint32_t was = ym.rhy;
    ym.bd = (uint8_t)v;
    ym.dam = (uint8_t)(v >> 7);
    ym.dvb = (uint8_t)((v >> 6) & 1);
    ym.rhy = (uint8_t)((v >> 5) & 1);
    if (!was && ym.rhy)
        ym.cb = (uint8_t)((ym.sl[0].P >> 22) & 0x28); /* spec 5.4 */
    for (int i = 12; i < 18; i++)
        set_key(&ym.sl[i], ym.sl[i].v->kon | drum(i));
}

static void nts_write(uint32_t v)
{
    const uint8_t n = (uint8_t)((v >> 6) & 1);
    if (n == ym.nts)
        return;
    ym.nts = n;
    for (int i = 0; i < 18; i++)
    {
        slot_t *s = &ym.sl[i];
        if (s->nsx)
        {
            s->r6w ^= 0x01010101;
            s->rc ^= 1;
        }
    }
}

/* Bit 7 rising zeroes the vibrato position after slot 0 has used it. */
static void test_write(uint32_t v)
{
    if ((v & ~(uint32_t)ym.test & 0x80) && (ym.vib & 3))
    {
        ym.v0fix = ym.vib;
        ym.vib = 0;
        force(ym.sl);
    }
    ym.test = (uint8_t)v;
}

/* one write; a is in ym3812_regs */
static void apply(uint32_t a, uint32_t v)
{
    YM3812_TRACE(a, v);
    switch (a >> 5)
    {
    case 0:
        if (a == 0x01)
            test_write(v);
        else
            nts_write(v);
        return;
    case 5:
    case 6:
    {
        int lo = a & 15, alias = 0;
        if (a == 0xbd)
        {
            bd_write(v);
            return;
        }
        if (lo >= 14)
            lo -= 14;
        else if (lo >= 9)
            lo -= 9, alias = 1;
        if (a >> 4 == 0xc)
            chan_c(lo, v, alias);
        else
            chan_ab(lo, a >> 4, v, alias);
        return;
    }
    }
    slot_t *s = &ym.sl[OFF2SLOT[a & 0x1f]];
    switch (a >> 5)
    {
    case 1:
        s->r20 = (uint8_t)v;
        s->ta = (s->ta & 0xffff) | ((v & 0x80) ? 0xff0000u : 0);
        slot_pg(s);
        slot_eg(s);
        return;
    case 2:
        s->r40 = (uint8_t)v;
        slot_ksl(s);
        return;
    case 3:
        s->r60 = (uint8_t)v;
        slot_eg(s);
        return;
    case 4:
        s->r80 = (uint8_t)v;
        slot_eg(s);
        return;
    default:
        if (ym.test & 0x20)
        {
            s->ws = v & 3;
            s->wav = WAV[v & 3];
        }
        return;
    }
}

/* ---------------- write path ---------------- */

/* The views that follow by one sample, then the oldest write if the credit
 * covers it. */
YM_COLD void commit(void)
{
    const uint32_t lag = ym.lag;
    if (lag)
    {
        ym.lag = 0;
        for (int c = 0; c < 3; c++)
            if (lag & (1u << c))
                mv_follow(c);
        for (int c = 3; c < 6; c++)
            if (lag & (1u << c))
            {
                ym.mv[c].fb = ym.cv[c].fb;
                chan_fb(c);
            }
    }
    ym3812_queue_t *q = &ym3812_queue;
    uint32_t credit = ym.credit;
    if (q->count && credit >= 96)
    {
        const uint32_t e = q->entry[q->head];
        q->head++;
        q->count--;
        credit -= 96;
        apply(e & 0xff, e >> 8);
    }
    credit += 72;
    if (!q->count && credit > 96)
        credit = 96;
    ym.credit = (uint8_t)credit;
}

/* ---------------- one sample ---------------- */

int16_t ym3812_sample(void)
{
    if (ym.lag | ym3812_queue.count | (ym.credit ^ 96u))
        commit();

    /* tremolo (spec 1.3) */
    const uint32_t test = ym.test;
    const uint32_t tout = ym.tv & 127;
    const int of = tout == 0 || (tout & 105) == 105;
    if (of && !ym.tof)
        ym.tdir ^= 1;
    ym.tof = (uint8_t)of;
    if (ym.tstep | (test & 8))
        ym.tv = (uint16_t)((ym.tv + (ym.tdir ? 511 : 1)) & 511);
    if (test & 0x80)
        ym.tv = 0, ym.tdir = 0;
    const uint32_t amv = ym.dam ? tout >> 2 : tout >> 4;
    const uint32_t vsh = VSH[ym.dvb][ym.vib];
    const uint32_t vs = 0u - (uint32_t)(ym.vib >> 2);

    /* step table for this sample (spec 3.3) */
    {
        uint32_t *w = ym.step;
        uint32_t *wa = w + ATTK / 4;
        if (ym.sub)
        {
            const uint8_t *x = RLW[ym.egshift];
            w[x[0]] = 0x01010101, wa[x[0]] = 0x02020202;
            w[x[1]] = 0x01010000, wa[x[1]] = 0x02020000;
            w[x[2]] = 0x01000100, wa[x[2]] = 0x02000200;
            w[17] = 0, wa[17] = 0;
            ym.rlw[0] = x[0], ym.rlw[1] = x[1], ym.rlw[2] = x[2];
        }
        else
        {
            const uint32_t r0 = ym.rlw[0], r1 = ym.rlw[1], r2 = ym.rlw[2];
            w[r0] = 0, w[r1] = 0, w[r2] = 0;
            wa[r0] = 0, wa[r1] = 0, wa[r2] = 0;
        }
        const uint32_t *h = HIROW[ym.sub][ym.eglow];
        for (int j = 0; j < 4; j++)
        {
            wa[12 + j] = h[j];
            w[12 + j] = h[j] >> 1;
        }
    }

    int32_t acc = ym.held15 + (ym.rhy ? 2 * ym.held1617 : ym.held1617);
    int32_t car;
    acc += chan(0, amv, vsh, vs, &car), acc += car;
    acc += chan(1, amv, vsh, vs, &car), acc += car;
    acc += chan(2, amv, vsh, vs, &car), acc += car;
    acc += chan(3, amv, vsh, vs, &car), acc += car;
    acc += chan(4, amv, vsh, vs, &car), acc += car;
    acc += chan(5, amv, vsh, vs, &car), acc += car;
    if (ym.rhy)
        acc += rhythm(amv, vsh, vs);
    else
    {
        int32_t c6, c7, c8;
        acc += chan(6, amv, vsh, vs, &c6);
        acc += chan(7, amv, vsh, vs, &c7);
        acc += chan(8, amv, vsh, vs, &c8);
        ym.held15 = c6;
        ym.held1617 = c7 + c8;
    }

    /* end of sample: counters (spec 1.3); test bit 7 holds them at 0 */
    const uint32_t lfo = ym.lfo;
    {
        uint32_t T = ym.T + (ym.carry | ym.sub);
        ym.carry = (uint8_t)(T >> 18);
        T &= 0x3ffff;
        ym.T = T;
        if (ym.sub)
        {
            ym.eglow = T & 3;
            const uint32_t t = T & 0x1fff;
            ym.egshift = (uint8_t)(t ? ym_ctz(t) + 1 : 0);
        }
        ym.sub ^= 1;
    }
    uint32_t N = ym.N;
    if (test & 0x80)
    {
        ym.tstep = 0;
        ym.vib = 0;
        ym.lfo = 0;
        N = (N << 18) | 0x3ffff;
    }
    else
    {
        ym.tstep = (lfo & 63) == 63;
        ym.vib = (uint8_t)((ym.vib + ((lfo == 1023) | ((test >> 3) & 1))) & 7);
        ym.lfo = (uint16_t)((lfo + 1) & 1023);
        N = ((N << 9) | (((N >> 14) ^ N) & 0x1ff)) & 0x7fffff;
        N = (N << 9) | (((N >> 14) ^ N) & 0x1ff);
    }
    ym.N = N & 0x7fffff;

    /* YM3014B (spec 6.2) */
    const int32_t s = SSAT(acc, 16);
    uint32_t t = (uint32_t)(s >> 9);
    t = (t ^ (uint32_t)((int32_t)t >> 31)) & 63;
    const uint32_t sh = t ? ym_bits(t) : 0;
    return (int16_t)(int32_t)((uint32_t)(s >> sh) << sh);
}

#pragma GCC pop_options

/* ---------------- reset and savestate ---------------- */

/* Every cache from the register shadow. */
static void rebuild(void)
{
    ym.dam = ym.bd >> 7;
    ym.dvb = (ym.bd >> 6) & 1;
    ym.rhy = (ym.bd >> 5) & 1;
    for (int c = 0; c < 9; c++)
    {
        view_k6(&ym.cv[c]);
        view_k6(&ym.mv[c]);
        chan_fb(c);
    }
    for (int i = 0; i < 18; i++)
    {
        slot_t *s = &ym.sl[i];
        s->v = is_mod(i) ? &ym.mv[SLOTCH[i]] : &ym.cv[SLOTCH[i]];
        s->key = (uint8_t)(s->v->kon | (i >= 12 && drum(i)));
        s->ta = (s->r20 & 0x80) ? 0xff0000u : 0;
        s->wav = WAV[s->ws];
        slot_pg(s);
        slot_ksl(s);
        slot_eg(s);
        eg_rec(s);
    }
}

void ym3812_reset(void)
{
    memset(&ym, 0, sizeof ym);
    memset(&ym3812_queue, 0, sizeof ym3812_queue);
    for (int i = 0; i < 18; i++)
    {
        ym.sl[i].level = 0x1ff;
        ym.sl[i].state = REL;
    }
    ym.tof = 1;
    ym.N = 1;
    ym.credit = 96;
    rebuild();
}

uint32_t ym3812_take_drops(void)
{
    const uint32_t d = ym3812_queue.drops;
    ym3812_queue.drops = 0;
    return d;
}

/* A view as its A0, B0 and C0 bytes. */
static void put_view(sst_cursor_t *c, const view_t *v)
{
    sst_put_u8(c, (uint8_t)v->fnum);
    sst_put_u8(c, (uint8_t)(v->kon << 5 | v->blk << 2 | v->fnum >> 8));
    sst_put_u8(c, (uint8_t)(v->fb << 1 | v->con));
}

static bool get_view(sst_cursor_t *c, view_t *v)
{
    const uint32_t a0 = sst_get_u8(c), b0 = sst_get_u8(c), c0 = sst_get_u8(c);
    v->fnum = (uint16_t)(a0 | (b0 & 3) << 8);
    v->blk = (b0 >> 2) & 7;
    v->kon = (b0 >> 5) & 1;
    v->con = c0 & 1;
    v->fb = (c0 >> 1) & 7;
    return b0 < 64 && c0 < 16;
}

static bool same_view(const view_t *a, const view_t *b, bool fb)
{
    return a->fnum == b->fnum && a->blk == b->blk && a->kon == b->kon &&
           a->con == b->con && (!fb || a->fb == b->fb);
}

static bool out13(uint32_t h)
{
    return (int16_t)h >= -4085 && (int16_t)h <= 4084;
}

void ym3812_sst_save(sst_cursor_t *c)
{
    for (int i = 0; i < 18; i++)
    {
        const slot_t *s = &ym.sl[i];
        sst_put_u32(c, s->P);
        sst_put_u16(c, (uint16_t)s->level);
        sst_put_u8(c, s->state);
        sst_put_u8(c, s->r20);
        sst_put_u8(c, s->r40);
        sst_put_u8(c, s->r60);
        sst_put_u8(c, s->r80);
        sst_put_u8(c, s->ws);
    }
    for (int i = 0; i < 9; i++)
    {
        sst_put_u32(c, ym.ch[i].hist);
        put_view(c, &ym.cv[i]);
        put_view(c, &ym.mv[i]);
    }
    sst_put_u8(c, ym.test);
    sst_put_u8(c, ym.nts);
    sst_put_u8(c, ym.bd);
    sst_put_u8(c, ym.lag);
    sst_put_u8(c, ym.vib);
    sst_put_u16(c, ym.lfo);
    sst_put_u16(c, ym.tv);
    sst_put_u8(c, ym.tdir);
    sst_put_u8(c, ym.tof);
    sst_put_u8(c, ym.tstep);
    sst_put_u32(c, ym.T);
    sst_put_u8(c, ym.carry);
    sst_put_u8(c, ym.sub);
    sst_put_u8(c, ym.egshift);
    sst_put_u8(c, ym.eglow);
    sst_put_u32(c, ym.N);
    sst_put_u8(c, ym.cb);
    sst_put_i32(c, ym.held15);
    sst_put_i32(c, ym.held1617);
    const ym3812_queue_t *q = &ym3812_queue;
    sst_put_u16(c, q->count);
    for (uint32_t i = 0; i < YM3812_QUEUE_SIZE; i++)
        sst_put_u16(c, i < q->count ? q->entry[(uint8_t)(q->head + i)] : 0);
    sst_put_u8(c, ym.credit);
    sst_put_u32(c, q->drops);
}

bool ym3812_sst_load(sst_cursor_t *c)
{
    memset(&ym, 0, sizeof ym);
    memset(&ym3812_queue, 0, sizeof ym3812_queue);
    bool ok = true;
    for (int i = 0; i < 18; i++)
    {
        slot_t *s = &ym.sl[i];
        s->P = sst_get_u32(c);
        s->level = sst_get_u16(c);
        s->state = sst_get_u8(c);
        s->r20 = sst_get_u8(c);
        s->r40 = sst_get_u8(c);
        s->r60 = sst_get_u8(c);
        s->r80 = sst_get_u8(c);
        s->ws = sst_get_u8(c);
        ok &= !(s->P & 0x1fff) && s->level <= 0x1ff && s->state <= REL && s->ws <= 3;
    }
    for (int i = 0; i < 9; i++)
    {
        ym.ch[i].hist = sst_get_u32(c);
        ok &= out13(ym.ch[i].hist) && out13(ym.ch[i].hist >> 16);
        ok &= get_view(c, &ym.cv[i]);
        ok &= get_view(c, &ym.mv[i]);
    }
    ym.test = sst_get_u8(c);
    ym.nts = sst_get_u8(c);
    ym.bd = sst_get_u8(c);
    ym.lag = sst_get_u8(c);
    ym.vib = sst_get_u8(c);
    ym.lfo = sst_get_u16(c);
    ym.tv = sst_get_u16(c);
    ym.tdir = sst_get_u8(c);
    ym.tof = sst_get_u8(c);
    ym.tstep = sst_get_u8(c);
    ym.T = sst_get_u32(c);
    ym.carry = sst_get_u8(c);
    ym.sub = sst_get_u8(c);
    ym.egshift = sst_get_u8(c);
    ym.eglow = sst_get_u8(c);
    ym.N = sst_get_u32(c);
    ym.cb = sst_get_u8(c);
    ym.held15 = sst_get_i32(c);
    ym.held1617 = sst_get_i32(c);
    ok &= ym.nts <= 1 && ym.lag <= 0x3f && ym.vib <= 7 && ym.lfo <= 1023 && ym.tv <= 511;
    ok &= ym.tdir <= 1 && ym.tof <= 1 && ym.tstep <= 1;
    ok &= ym.T <= 0x3ffff && ym.carry <= 1 && ym.sub <= 1 && ym.egshift <= 13 && ym.eglow <= 3;
    ok &= ym.N && ym.N <= 0x7fffff && !(ym.cb & ~0x28);
    ok &= ym.held15 >= -8170 && ym.held15 <= 8168 && ym.held1617 >= -8170 && ym.held1617 <= 8168;
    /* the modulator view differs from the carrier's only while it lags */
    for (int i = 0; i < 9; i++)
        ok &= same_view(&ym.mv[i], &ym.cv[i], !(ym.lag & (1u << i))) ||
              (i < 3 && (ym.lag & (1u << i)));
    ym3812_queue_t *q = &ym3812_queue;
    q->count = sst_get_u16(c);
    ok &= q->count <= YM3812_QUEUE_SIZE;
    for (uint32_t i = 0; i < YM3812_QUEUE_SIZE; i++)
    {
        const uint32_t e = sst_get_u16(c);
        q->entry[i] = (uint16_t)e;
        ok &= i >= q->count || ((ym3812_regs[(e & 0xff) >> 3] >> (e & 7)) & 1);
    }
    ym.credit = sst_get_u8(c);
    q->drops = sst_get_u32(c);
    ok &= ym.credit <= 144 && !(ym.credit % 24) && (q->count || ym.credit <= 96);
    ok &= q->drops <= 0x7FFFFFFF;
    if (!ok || !sst_ok(c))
    {
        ym3812_reset();
        return false;
    }
    rebuild();
    return true;
}
