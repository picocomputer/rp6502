/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Section numbers refer to opl.md, the per-sample YM3812 specification.
 *
 * A write is applied before the sample it first affects (spec 7.6). Its
 * effects are cached where they are needed: per slot the phase increment of
 * each vibrato variant, the KSL + TL term, the rate indices and the waveform;
 * per channel the views the slots read. Each slot keeps an envelope record for
 * its state (rate code, level range); inside the range a sample is one table
 * step, leaving it is a stage change, and span 0 makes the slot re-evaluate the
 * whole rule next. */

#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include <string.h>

/* A test that compiles this file defines this to see each write as it is
 * applied. */
#ifndef OPL_TRACE
#define OPL_TRACE(reg, val)
#endif

#pragma GCC push_options
#pragma GCC optimize("O3")

#if defined(_MSC_VER)
#include <intrin.h>
#define OPL_HOT static __forceinline
#define OPL_COLD static __declspec(noinline)
static inline uint32_t opl_ctz(uint32_t x)
{
    unsigned long i;
    _BitScanForward(&i, x);
    return (uint32_t)i;
}
static inline uint32_t opl_bits(uint32_t x)
{
    unsigned long i;
    _BitScanReverse(&i, x);
    return (uint32_t)i + 1;
}
#else
#define OPL_HOT static inline __attribute__((always_inline))
#define OPL_COLD static __attribute__((noinline))
#define opl_ctz(x) ((uint32_t)__builtin_ctz(x))
#define opl_bits(x) (32 - (uint32_t)__builtin_clz(x))
#endif

#if defined(__GNUC__) && defined(__arm__)
#define OPL_USAT(x, n) ((uint32_t)__builtin_arm_usat((int32_t)(x), (n)))
#define OPL_SSAT(x, n) __builtin_arm_ssat((int32_t)(x), (n))
/* A register shift by 32 or more gives 0. */
static inline uint32_t opl_lslr(uint32_t x, uint32_t n)
{
    uint32_t r;
    __asm__("lsl %0, %1, %2" : "=r"(r) : "r"(x), "r"(n));
    return r;
}
#else
static inline uint32_t OPL_USAT(int32_t x, int n)
{
    const int32_t m = (1 << n) - 1;
    return (uint32_t)(x < 0 ? 0 : x > m ? m : x);
}
static inline int32_t OPL_SSAT(int32_t x, int n)
{
    const int32_t m = (1 << (n - 1)) - 1;
    return x > m ? m : x < -m - 1 ? -m - 1 : x;
}
static inline uint32_t opl_lslr(uint32_t x, uint32_t n)
{
    return n >= 32 ? 0 : x << n;
}
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "a step word holds rate rhi * 4 + n in its byte n");
#endif
_Static_assert(OPL_QUEUE_SIZE == 256, "the queue head wraps as a uint8_t");
_Static_assert(AUD_NATIVE_RATE == 49716, "one chip sample is 72 master clocks");

/* log-sin[256], then (exp | 1024) << 1 [256] (spec 4.1) */
static const uint16_t opl_rom[512] = {
#include "opl_rom.h"
};

enum
{
    OPL_ATT,
    OPL_DEC,
    OPL_SUS,
    OPL_REL
};
#define OPL_NONE 64 /* rate index with no step */
#define OPL_ATTK 72 /* rate codes from here are the attack form */

/* A slot's view of its channel's F-number, block and KON, and CON, FB. */
typedef struct
{
    uint16_t fnum;
    uint8_t blk, kon, con, fb;
    uint8_t k6; /* KSL attenuation before the KSL shift (spec 3.5) */
} opl_view_t;

typedef struct
{
    uint32_t P;        /* 19-bit phase accumulator << 13 */
    int32_t level;     /* 9-bit attenuation */
    union
    {
        struct
        {
            uint16_t lo, span; /* fast path while (level - lo) < span; span 0: re-evaluate */
        };
        uint32_t rec; /* lo | span << 16 */
    };
    uint8_t rc;        /* rate code: step table index, OPL_ATTK + rate index in attack */
    uint8_t state;
    uint8_t key;
    uint8_t qm;      /* muted at a fixed point */
    uint32_t ta;     /* KSL + 4 TL | AM mask (0 or 0xff) << 16 */
    uint32_t wav;    /* zero mask (0x100 or 0x200) | 1 << 31 for the NOT half */
    int32_t mute;    /* -1 summed, 0 muted, this sample */
    uint32_t inc[7]; /* increment << 13 per vibrato variant (opl_vmap) */
    uint32_t r6w;    /* rate index per state, byte per state */
    uint32_t drec;   /* rec of decay from level 0 */
    uint8_t slv;     /* SL, 31 for 15 */
    uint8_t nsx;     /* NTS flips bit 0 of the rate indices */
    uint8_t r20, r40, r60, r80, ws;
    const opl_view_t *v; /* the channel view this slot reads (spec 7.6) */
} opl_slot_t;

typedef struct
{
    uint32_t hist; /* modulator's last output << 16 | the one before */
    int32_t incl;  /* -1 when CON sums the modulator */
    uint8_t fbl;   /* 13 + FB, or 32 for FB = 0 */
} opl_chan_t;

typedef struct
{
    uint32_t step[36]; /* bytes [r6]: linear step, [OPL_ATTK + r6]: attack multiplier */
    opl_slot_t sl[18];
    opl_chan_t ch[9];
    opl_view_t cv[9], mv[9]; /* carrier view, modulator view (spec 7.6) */
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
} opl_t;

static opl_t opl_chip;
opl_queue_t opl_queue;

const uint8_t opl_regs[32] = {
    0x02, 0x01, 0x00, 0x00, 0x3F, 0x3F, 0x3F, 0x00, /* 01, 08, 20-35 */
    0x3F, 0x3F, 0x3F, 0x00, 0x3F, 0x3F, 0x3F, 0x00, /* 40-55, 60-75 */
    0x3F, 0x3F, 0x3F, 0x00, 0xFF, 0xCF, 0xFF, 0xEF, /* 80-95, A0-AB AE AF, B0-BB BD-BF */
    0xFF, 0xCF, 0x00, 0x00, 0x3F, 0x3F, 0x3F, 0x00, /* C0-CB CE CF, E0-F5 */
};

static const uint8_t opl_mt[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
static const uint8_t opl_kslrom[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
static const uint8_t opl_kslmul[4] = {0, 2, 1, 4};
/* operator offset & 0x1f -> slot; 6, 7, 14 and 15 are not operator offsets */
static const uint8_t opl_off2slot[22] = {0, 1, 2, 3, 4, 5, 0, 0, 6, 7, 8, 9, 10, 11, 0, 0,
                                     12, 13, 14, 15, 16, 17};
static const uint8_t opl_slotch[18] = {0, 1, 2, 0, 1, 2, 3, 4, 5, 3, 4, 5, 6, 7, 8, 6, 7, 8};
static const uint8_t opl_mslot[9] = {0, 1, 2, 6, 7, 8, 12, 13, 14};
static const uint8_t opl_drumbit[6] = {16, 1, 4, 16, 8, 2};
/* vibrato variant per [DVB][position]: 0 f, 1 f+c, 2 f+b, 3 f+a, 4 f-c, 5 f-b,
 * 6 f-a, where a, b, c are the F-number's top 3, 2, 1 bits (spec 2) */
static const uint8_t opl_vmap[2][8] = {{0, 1, 2, 1, 0, 4, 5, 4}, {0, 2, 3, 2, 0, 5, 6, 5}};
/* attack multipliers (2, 4, 8 for step sizes 1, 2, 4) for rates 48..63 by [sub][eglow];
 * the linear steps are half of these */
static const uint32_t opl_hirow[2][4][4] = {
    {{0x02020200, 0x04040402, 0x08080804, 0x08080808},
     {0x02000000, 0x04020202, 0x08040404, 0x08080808},
     {0x02020000, 0x04040202, 0x08080404, 0x08080808},
     {0x00000000, 0x02020202, 0x04040404, 0x08080808}},
    {{0x02020202, 0x04040402, 0x08080804, 0x08080808},
     {0x02020202, 0x04020202, 0x08040404, 0x08080808},
     {0x02020202, 0x04040202, 0x08080404, 0x08080808},
     {0x02020202, 0x02020202, 0x04040404, 0x08080808}}};
/* step words for rates below 48 by egshift on sub = 1 samples (17: unused word) */
static const uint8_t opl_rlw[14][3] = {
    {17, 17, 17}, {11, 17, 17}, {10, 11, 17}, {9, 10, 11}, {8, 9, 10}, {7, 8, 9}, {6, 7, 8},
    {5, 6, 7}, {4, 5, 6}, {3, 4, 5}, {2, 3, 4}, {1, 2, 3}, {0, 1, 2}, {17, 0, 1}};
static const uint32_t opl_wav[4] = {0x80000000u, 0x200, 0, 0x100};

#define OPL_STEP(i) (((const uint8_t *)opl_chip.step)[i])

static inline int opl_is_mod(int s) { return (s % 6) < 3; }

static inline uint32_t opl_r6(const opl_slot_t *s, uint32_t st)
{
    return (s->r6w >> (st * 8)) & 0xff;
}

/* ---------------- envelope events ---------------- */

static void opl_eg_muted(opl_slot_t *s)
{
    s->level = 0x1ff, s->mute = 0;
    s->lo = 0x1ff, s->span = 1, s->rc = OPL_NONE, s->qm = 1;
}

/* Record for the state and level, key consistent with the state. A level
 * at a transition point gets a range that excludes it. */
static void opl_eg_rec(opl_slot_t *s)
{
    const int st = s->state, lv = s->level;
    s->mute = -1, s->qm = 0;
    if (st == OPL_ATT)
    {
        const uint32_t a = opl_r6(s, OPL_ATT);
        s->lo = 1, s->span = 0x1ff;
        s->rc = (uint8_t)(OPL_ATTK + (a >= 60 ? OPL_NONE : a));
        return;
    }
    if (lv == 0x1ff && (st != OPL_DEC || s->slv != 31))
    {
        opl_eg_muted(s);
        return;
    }
    s->rc = (uint8_t)opl_r6(s, (uint32_t)st);
    int lo = 0, top = 0x1f8;
    if (st == OPL_DEC)
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
OPL_COLD void opl_eg_forced(opl_slot_t *s, uint32_t inc)
{
    if (opl_chip.v0fix)
    {
        /* test bit 7 rose: slot 0, forced and run first, still steps at the
         * old vibrato position */
        s->P += s->inc[opl_vmap[opl_chip.dvb][opl_chip.v0fix]] - inc;
        opl_chip.v0fix = 0;
    }
    const int st = s->state, k = s->key, lv = s->level;
    if (st == OPL_REL && k)
    {
        /* key-on edge: no step, phase restarts, attack from this level */
        if (opl_r6(s, OPL_ATT) - 60 < 4)
            s->level = 0;
        s->state = OPL_ATT;
        s->P = inc;
        opl_eg_rec(s);
        return;
    }
    if (st != OPL_REL && !k)
    {
        /* key-off: this sample steps with the old state's rate */
        int mute = -1;
        if (st != OPL_ATT)
        {
            if (lv >= 0x1f8)
                s->level = 0x1ff, mute = 0;
            else if (st == OPL_SUS || (lv >> 4) != s->slv)
                s->level = lv + OPL_STEP(opl_r6(s, (uint32_t)st));
        }
        s->state = OPL_REL;
        opl_eg_rec(s);
        if (s->mute != mute)
            s->lo = 0x200; /* in the mix this sample, muted from the next */
        s->mute = mute;
        return;
    }
    opl_eg_rec(s);
    if ((uint32_t)(lv - s->lo) < s->span)
    {
        const uint32_t rc = s->rc;
        int32_t d = OPL_STEP(rc);
        if (rc >= OPL_ATTK)
            d = (~lv * d) >> 4;
        s->level = lv + d;
        return;
    }
    s->span = 2; /* the level is at a transition point: take it below */
}

/* The level left its range: attack reached 0, decay reached the SL band, or
 * the level reached 0x1f8. No step this sample (spec 3.4). */
OPL_COLD void opl_eg_event(opl_slot_t *s, uint32_t inc)
{
    if (!s->span)
    {
        opl_eg_forced(s, inc);
        if (s->span != 2)
            return;
    }
    const int st = s->state, lv = s->level;
    if (st == OPL_ATT)
    {
        s->state = OPL_DEC;
        s->rec = s->drec, s->rc = (uint8_t)opl_r6(s, OPL_DEC);
        return;
    }
    if (st == OPL_DEC && (lv >> 4) == s->slv)
    {
        s->state = OPL_SUS;
        if (lv < 0x1f8)
        {
            s->lo = 0, s->span = 0x1f8, s->rc = (uint8_t)opl_r6(s, OPL_SUS);
            return;
        }
    }
    opl_eg_muted(s);
}

/* ---------------- per-sample kernels ---------------- */

OPL_HOT uint32_t opl_eg(opl_slot_t *s, uint32_t amv, uint32_t inc)
{
    const int32_t e = s->level;
    const uint32_t ta = s->ta;
    const uint32_t eo = OPL_USAT(e + (int32_t)(ta & 0xffff) + (int32_t)(amv & (ta >> 16)), 9);
    if ((uint32_t)(e - s->lo) < s->span)
    {
        const uint32_t rc = s->rc;
        int32_t d = OPL_STEP(rc);
        if (rc >= OPL_ATTK)
            d = (~e * d) >> 4;
        s->level = e + d;
    }
    else if (s->span)
    {
        /* stage change, no step this sample */
        const int st = s->state;
        if (st == OPL_ATT)
        {
            s->state = OPL_DEC;
            s->rec = s->drec, s->rc = (uint8_t)opl_r6(s, OPL_DEC);
        }
        else if (st == OPL_DEC && (e >> 4) == s->slv && e < 0x1f8)
        {
            s->state = OPL_SUS;
            s->lo = 0, s->span = 0x1f8, s->rc = (uint8_t)opl_r6(s, OPL_SUS);
        }
        else
        {
            if (st == OPL_DEC && (e >> 4) == s->slv)
                s->state = OPL_SUS;
            s->level = 0x1ff, s->mute = 0;
            s->lo = 0x1ff, s->span = 1, s->rc = OPL_NONE, s->qm = 1;
        }
    }
    else
        opl_eg_event(s, inc);
    return eo;
}

OPL_HOT int32_t opl_op(const opl_slot_t *s, uint32_t p, uint32_t eo)
{
    p &= 0x3ff;
    const uint32_t idx = (p ^ (uint32_t)((int32_t)(p << 23) >> 31)) & 0xff;
    const uint32_t att = OPL_USAT((int32_t)(opl_rom[idx] + (eo << 3)), 12);
    int32_t v = (int32_t)(opl_rom[256 + (att & 0xff)] >> (att >> 8));
    const uint32_t w = s->wav;
    if (p & w)
        v = 0;
    return v ^ ((int32_t)(w & (p << 22)) >> 31);
}

/* One melodic channel: returns the modulator's contribution, the carrier's
 * in *car (channels 6-8 hold it for the next output). */
OPL_HOT int32_t opl_chan(const int c, uint32_t amv, uint32_t vi, int32_t *car)
{
    opl_slot_t *m = &opl_chip.sl[opl_mslot[c]], *k = m + 3;
    opl_chan_t *h = &opl_chip.ch[c];
    const uint32_t pm = m->P, pk = k->P;
    const uint32_t im = m->inc[vi], ik = k->inc[vi];
    m->P = pm + im;
    k->P = pk + ik;
    const uint32_t hs = h->hist;
    const int32_t h1 = (int32_t)hs >> 16;
    const uint32_t fb = opl_lslr((uint32_t)(h1 + (int16_t)hs), h->fbl) >> 22;
    if (m->qm & k->qm)
    {
        const int32_t mo = (int32_t)(m->wav & (((pm >> 22) + fb) << 22)) >> 31;
        h->hist = ((uint32_t)h1 & 0xffff) | (uint32_t)mo << 16;
        *car = 0;
        return 0;
    }
    const uint32_t em = opl_eg(m, amv, im);
    const int32_t mo = opl_op(m, (pm >> 22) + fb, em);
    h->hist = ((uint32_t)h1 & 0xffff) | (uint32_t)mo << 16;
    const uint32_t ek = opl_eg(k, amv, ik);
    const int32_t ko = opl_op(k, (pk >> 22) + (uint32_t)(mo & ~h->incl), ek);
    *car = ko & k->mute;
    return mo & m->mute & h->incl;
}

/* Channels 6-8 in rhythm mode (spec 5): returns HH + TOM, holds BD, SD, TC. */
OPL_HOT int32_t opl_rhythm(uint32_t amv, uint32_t vi)
{
    opl_slot_t *s = opl_chip.sl;
    int32_t acc;
    {
        opl_slot_t *m = &s[12], *k = &s[15];
        opl_chan_t *h = &opl_chip.ch[6];
        const uint32_t pm = m->P, pk = k->P;
        const uint32_t im = m->inc[vi], ik = k->inc[vi];
        m->P = pm + im;
        k->P = pk + ik;
        const uint32_t hs = h->hist;
        const int32_t h1 = (int32_t)hs >> 16;
        const uint32_t fb = opl_lslr((uint32_t)(h1 + (int16_t)hs), h->fbl) >> 22;
        const uint32_t em = opl_eg(m, amv, im);
        const int32_t mo = opl_op(m, (pm >> 22) + fb, em);
        h->hist = ((uint32_t)h1 & 0xffff) | (uint32_t)mo << 16;
        const uint32_t ek = opl_eg(k, amv, ik);
        const int32_t ko = opl_op(k, (pk >> 22) + (uint32_t)(mo & ~h->incl), ek);
        opl_chip.held15 = 2 * (ko & k->mute);
    }
    const uint32_t N = opl_chip.N;
    uint32_t hb;
    {
        opl_slot_t *m = &s[13];
        const uint32_t p = m->P, i = m->inc[vi];
        m->P = p + i;
        const uint32_t e = opl_eg(m, amv, i);
        hb = p >> 22;
        const uint32_t cb = opl_chip.cb;
        const uint32_t rm = (((hb >> 2) ^ (hb >> 7)) | ((hb >> 3) ^ (cb >> 5)) | ((cb >> 3) ^ (cb >> 5))) & 1;
        const uint32_t hp = (rm << 9) | ((rm ^ (N >> 9)) & 1 ? 0xd0 : 0x34);
        acc = 2 * (opl_op(m, hp, e) & m->mute);
    }
    {
        opl_slot_t *k = &s[16];
        const uint32_t p = k->P, i = k->inc[vi];
        k->P = p + i;
        const uint32_t e = opl_eg(k, amv, i);
        const uint32_t h8 = (hb >> 8) & 1;
        opl_chip.held1617 = opl_op(k, (h8 << 9) | (((h8 ^ (N >> 6)) & 1) << 8), e) & k->mute;
    }
    {
        opl_slot_t *m = &s[14];
        const uint32_t p = m->P, i = m->inc[vi];
        m->P = p + i;
        const uint32_t e = opl_eg(m, amv, i);
        acc += 2 * (opl_op(m, p >> 22, e) & m->mute);
    }
    {
        opl_slot_t *k = &s[17];
        const uint32_t p = k->P, i = k->inc[vi];
        k->P = p + i;
        const uint32_t e = opl_eg(k, amv, i);
        const uint32_t cb = p >> 22;
        opl_chip.cb = (uint8_t)(cb & 0x28);
        const uint32_t rm = (((hb >> 2) ^ (hb >> 7)) | ((hb >> 3) ^ (cb >> 5)) | ((cb >> 3) ^ (cb >> 5))) & 1;
        opl_chip.held1617 += opl_op(k, (rm << 9) | 0x100, e) & k->mute;
    }
    return acc;
}

/* ---------------- write application ---------------- */

static inline void opl_force(opl_slot_t *s)
{
    s->span = 0;
    s->qm = 0;
}

static inline void opl_set_key(opl_slot_t *s, uint32_t k)
{
    if (s->key != k)
    {
        s->key = (uint8_t)k;
        opl_force(s);
    }
}

static void opl_view_k6(opl_view_t *v)
{
    const int k6 = opl_kslrom[v->fnum >> 6] - 8 * (8 - v->blk);
    v->k6 = (uint8_t)(k6 < 0 ? 0 : k6);
}

/* inc << 13 = ((((f << blk) >> 1) * mt) >> 1) << 13 = ((f & km) * (mt << (11 + blk)))
 * with bit 12 cleared, km clearing bit 0 when blk = 0 (mod 2^32), at each
 * vibrato variant of f; all seven are f's without VIB */
static void opl_slot_pg(opl_slot_t *s)
{
    const opl_view_t *v = s->v;
    const uint32_t f = v->fnum, bk = v->blk;
    const uint32_t mul = (uint32_t)opl_mt[s->r20 & 15] << (11 + bk), km = bk ? ~0u : ~1u;
    const uint32_t a = (s->r20 & 0x40) ? f >> 7 : 0;
    const uint32_t d[7] = {0, a >> 2, a >> 1, a, 0u - (a >> 2), 0u - (a >> 1), 0u - a};
    for (int i = 0; i < 7; i++)
        s->inc[i] = (((f + d[i]) & km) * mul) & ~0x1000u;
}

static inline void opl_slot_ksl(opl_slot_t *s)
{
    s->ta = (s->ta & 0xffff0000u) | (uint32_t)(s->v->k6 * opl_kslmul[s->r40 >> 6] + ((s->r40 & 63) << 2));
}

/* rate index per state, bytewise: 4R + ks, 60 + (ks & 3) above 63, OPL_NONE for R = 0 */
static void opl_slot_eg(opl_slot_t *s)
{
    const opl_view_t *v = s->v;
    const uint32_t r20 = s->r20, r60 = s->r60, r80 = s->r80;
    const uint32_t ksr = r20 & 0x10;
    const uint32_t ns = (v->fnum >> (opl_chip.nts ? 8 : 9)) & 1;
    const uint32_t ks = ksr ? (uint32_t)(v->blk << 1) | ns : (uint32_t)v->blk >> 1;
    const uint32_t rr = r80 & 15;
    const uint32_t R = (r60 >> 4) | (r60 & 15) << 8 | ((r20 & 0x20) ? 0 : rr) << 16 | rr << 24;
    uint32_t x = (R << 2) + ks * 0x01010101u;            /* bytes <= 75 */
    const uint32_t ov = ((x >> 6) & 0x01010101u) * 0xff; /* bytes > 63 */
    x = (x & ~ov) | ((60 + (ks & 3)) * 0x01010101u & ov);
    const uint32_t nz = (R | R >> 1 | R >> 2 | R >> 3) & 0x01010101u;
    const uint32_t z = (nz ^ 0x01010101u) * 0xff; /* bytes with R = 0 */
    const uint32_t w = (x & ~z) | (OPL_NONE * 0x01010101u & z);
    const uint32_t slv = (r80 >> 4) == 15 ? 31 : r80 >> 4;
    s->nsx = (uint8_t)(ksr && (((v->fnum >> 8) ^ (v->fnum >> 9)) & 1));
    if (w != s->r6w || slv != s->slv)
    {
        s->r6w = w;
        s->slv = (uint8_t)slv;
        if (s->span)
            opl_eg_rec(s);
    }
}

/* decay from level 0 up to the SL band; SL 0: none (lo 0x200, span 1) */
static inline void opl_slot_drec(opl_slot_t *s)
{
    s->drec = s->slv ? (uint32_t)s->slv << 20 : 0x10200;
}

static inline uint32_t opl_drum(int i)
{
    return opl_chip.rhy && (opl_chip.bd & opl_drumbit[i - 12]);
}

/* a slot's view changed: what = 1 F-number low bits, 3 F-number high bits
 * or block, | 4 key; the key settles first so a key change rebuilds once */
static void opl_slot_view(int i, int what)
{
    opl_slot_t *s = &opl_chip.sl[i];
    if (what & 4)
        opl_set_key(s, s->v->kon | (i >= 12 && opl_drum(i)));
    if (what & 1)
    {
        opl_slot_pg(s);
        opl_slot_ksl(s);
        if (what & 2)
            opl_slot_eg(s);
    }
}

static inline void opl_chan_fb(int c)
{
    opl_chan_t *h = &opl_chip.ch[c];
    const opl_view_t *v = &opl_chip.mv[c];
    h->incl = v->con ? -1 : 0;
    h->fbl = (uint8_t)(v->fb ? 13 + v->fb : 32);
}

/* A0: F-number low bits. Returns 1 if they changed. */
static inline int opl_view_a(opl_view_t *v, uint32_t d)
{
    const uint32_t f = (v->fnum & 0x300) | d;
    if (f == v->fnum)
        return 0;
    const uint32_t hi = (f ^ v->fnum) >> 6;
    v->fnum = (uint16_t)f;
    if (hi)
        opl_view_k6(v);
    return 1;
}

/* B0: returns 3 if F-number high bits or block changed, | 4 for the key */
static inline int opl_view_b(opl_view_t *v, uint32_t d)
{
    const uint32_t f = (v->fnum & 0xff) | (d & 3) << 8, b = (d >> 2) & 7, k = (d >> 5) & 1;
    int what = 0;
    if (f != v->fnum || b != v->blk)
    {
        v->fnum = (uint16_t)f, v->blk = (uint8_t)b;
        opl_view_k6(v);
        what = 3;
    }
    if (k != v->kon)
        v->kon = (uint8_t)k, what |= 4;
    return what;
}

/* channel 0-2: the modulator view becomes the carrier view */
static void opl_mv_follow(int c)
{
    opl_view_t *mv = &opl_chip.mv[c];
    const opl_view_t *cv = &opl_chip.cv[c];
    int what = 0;
    if (mv->fnum != cv->fnum || mv->blk != cv->blk)
        what = (mv->blk != cv->blk || ((mv->fnum ^ cv->fnum) >> 8)) ? 3 : 1;
    if (mv->kon != cv->kon)
        what |= 4;
    *mv = *cv;
    if (what)
        opl_slot_view(opl_mslot[c], what);
    opl_chan_fb(c);
}

/* A0-A8 / B0-B8: alias 1 for A9-AB / B9-BB (both slots now, spec 7.6) */
static void opl_chan_ab(int c, uint32_t g, uint32_t d, int alias)
{
    const int ms = opl_mslot[c];
    opl_view_t *cv = &opl_chip.cv[c];
    const int what = g == 0xa ? opl_view_a(cv, d) : opl_view_b(cv, d);
    if (what)
        opl_slot_view(ms + 3, what);
    if (c < 3)
    {
        if (!alias)
            opl_chip.lag |= (uint8_t)(1 << c);
        else
        {
            opl_view_t *mv = &opl_chip.mv[c];
            const int w2 = g == 0xa ? opl_view_a(mv, d) : opl_view_b(mv, d);
            if (w2)
                opl_slot_view(ms, w2);
        }
        return;
    }
    if (what)
    {
        opl_view_t *mv = &opl_chip.mv[c];
        mv->fnum = cv->fnum, mv->blk = cv->blk, mv->kon = cv->kon, mv->k6 = cv->k6;
        opl_slot_view(ms, what);
    }
}

/* C0-C8: FB and CON; alias 1 for C9-CB */
static void opl_chan_c(int c, uint32_t d, int alias)
{
    opl_view_t *cv = &opl_chip.cv[c], *mv = &opl_chip.mv[c];
    cv->con = d & 1;
    cv->fb = (uint8_t)((d >> 1) & 7);
    if (c < 3 && !alias)
    {
        opl_chip.lag |= (uint8_t)(1 << c);
        return;
    }
    mv->con = cv->con;
    if (c >= 3 && c < 6)
        opl_chip.lag |= (uint8_t)(1 << c);
    else
        mv->fb = cv->fb;
    opl_chan_fb(c);
}

static void opl_bd_write(uint32_t v)
{
    const uint32_t was = opl_chip.rhy;
    opl_chip.bd = (uint8_t)v;
    opl_chip.dam = (uint8_t)(v >> 7);
    opl_chip.dvb = (uint8_t)((v >> 6) & 1);
    opl_chip.rhy = (uint8_t)((v >> 5) & 1);
    if (!was && opl_chip.rhy)
        opl_chip.cb = (uint8_t)((opl_chip.sl[0].P >> 22) & 0x28); /* spec 5.4 */
    for (int i = 12; i < 18; i++)
        opl_set_key(&opl_chip.sl[i], opl_chip.sl[i].v->kon | opl_drum(i));
}

static void opl_nts_write(uint32_t v)
{
    const uint8_t n = (uint8_t)((v >> 6) & 1);
    if (n == opl_chip.nts)
        return;
    opl_chip.nts = n;
    for (int i = 0; i < 18; i++)
    {
        opl_slot_t *s = &opl_chip.sl[i];
        if (s->nsx)
        {
            s->r6w ^= 0x01010101;
            s->rc ^= 1;
        }
    }
}

/* Bit 7 rising zeroes the vibrato position after slot 0 has used it. */
static void opl_test_write(uint32_t v)
{
    if ((v & ~(uint32_t)opl_chip.test & 0x80) && (opl_chip.vib & 3))
    {
        opl_chip.v0fix = opl_chip.vib;
        opl_chip.vib = 0;
        opl_force(opl_chip.sl);
    }
    opl_chip.test = (uint8_t)v;
}

/* one write; a is in opl_regs */
static void opl_apply(uint32_t a, uint32_t v)
{
    OPL_TRACE(a, v);
    switch (a >> 5)
    {
    case 0:
        if (a == 0x01)
            opl_test_write(v);
        else
            opl_nts_write(v);
        return;
    case 5:
    case 6:
    {
        int lo = a & 15, alias = 0;
        if (a == 0xbd)
        {
            opl_bd_write(v);
            return;
        }
        if (lo >= 14)
            lo -= 14;
        else if (lo >= 9)
            lo -= 9, alias = 1;
        if (a >> 4 == 0xc)
            opl_chan_c(lo, v, alias);
        else
            opl_chan_ab(lo, a >> 4, v, alias);
        return;
    }
    }
    opl_slot_t *s = &opl_chip.sl[opl_off2slot[a & 0x1f]];
    switch (a >> 5)
    {
    case 1:
        s->r20 = (uint8_t)v;
        s->ta = (s->ta & 0xffff) | ((v & 0x80) ? 0xff0000u : 0);
        opl_slot_pg(s);
        opl_slot_eg(s);
        return;
    case 2:
        s->r40 = (uint8_t)v;
        opl_slot_ksl(s);
        return;
    case 3:
        s->r60 = (uint8_t)v;
        opl_slot_eg(s);
        return;
    case 4:
        s->r80 = (uint8_t)v;
        opl_slot_eg(s);
        opl_slot_drec(s);
        return;
    default:
        if (opl_chip.test & 0x20)
        {
            s->ws = v & 3;
            s->wav = opl_wav[v & 3];
        }
        return;
    }
}

/* ---------------- write path ---------------- */

/* The views that follow by one sample, then the oldest write if the credit
 * covers it and no channel 0-2 late half was applied. */
OPL_COLD void opl_commit(void)
{
    const uint32_t lag = opl_chip.lag;
    if (lag)
    {
        opl_chip.lag = 0;
        for (int c = 0; c < 3; c++)
            if (lag & (1u << c))
                opl_mv_follow(c);
        for (int c = 3; c < 6; c++)
            if (lag & (1u << c))
            {
                opl_chip.mv[c].fb = opl_chip.cv[c].fb;
                opl_chan_fb(c);
            }
    }
    opl_queue_t *q = &opl_queue;
    uint32_t credit = opl_chip.credit;
    if (!(lag & 7) && q->count && credit >= 96)
    {
        const uint32_t e = q->entry[q->head];
        q->head++;
        q->count--;
        credit -= 96;
        opl_apply(e & 0xff, e >> 8);
    }
    credit += 72;
    const uint32_t cap = q->count ? 192 : 96;
    opl_chip.credit = (uint8_t)(credit > cap ? cap : credit);
}

/* ---------------- one sample ---------------- */

int16_t opl_sample(void)
{
    if (opl_chip.lag | opl_queue.count | (opl_chip.credit ^ 96u))
        opl_commit();

    /* tremolo (spec 1.3) */
    const uint32_t test = opl_chip.test;
    const uint32_t tout = opl_chip.tv & 127;
    const int of = tout == 0 || (tout & 105) == 105;
    if (of && !opl_chip.tof)
        opl_chip.tdir ^= 1;
    opl_chip.tof = (uint8_t)of;
    if (opl_chip.tstep | (test & 8))
        opl_chip.tv = (uint16_t)((opl_chip.tv + (opl_chip.tdir ? 511 : 1)) & 511);
    if (test & 0x80)
        opl_chip.tv = 0, opl_chip.tdir = 0;
    const uint32_t amv = opl_chip.dam ? tout >> 2 : tout >> 4;
    const uint32_t vi = opl_vmap[opl_chip.dvb][opl_chip.vib];

    /* step table for this sample (spec 3.3) */
    {
        uint32_t *w = opl_chip.step;
        uint32_t *wa = w + OPL_ATTK / 4;
        if (opl_chip.sub)
        {
            const uint8_t *x = opl_rlw[opl_chip.egshift];
            w[x[0]] = 0x01010101, wa[x[0]] = 0x02020202;
            w[x[1]] = 0x01010000, wa[x[1]] = 0x02020000;
            w[x[2]] = 0x01000100, wa[x[2]] = 0x02000200;
            w[17] = 0, wa[17] = 0;
            opl_chip.rlw[0] = x[0], opl_chip.rlw[1] = x[1], opl_chip.rlw[2] = x[2];
        }
        else
        {
            const uint32_t r0 = opl_chip.rlw[0], r1 = opl_chip.rlw[1], r2 = opl_chip.rlw[2];
            w[r0] = 0, w[r1] = 0, w[r2] = 0;
            wa[r0] = 0, wa[r1] = 0, wa[r2] = 0;
        }
        const uint32_t *h = opl_hirow[opl_chip.sub][opl_chip.eglow];
        for (int j = 0; j < 4; j++)
        {
            wa[12 + j] = h[j];
            w[12 + j] = h[j] >> 1;
        }
    }

    int32_t acc = opl_chip.held15 + (opl_chip.rhy ? 2 * opl_chip.held1617 : opl_chip.held1617);
    int32_t car;
    acc += opl_chan(0, amv, vi, &car), acc += car;
    acc += opl_chan(1, amv, vi, &car), acc += car;
    acc += opl_chan(2, amv, vi, &car), acc += car;
    acc += opl_chan(3, amv, vi, &car), acc += car;
    acc += opl_chan(4, amv, vi, &car), acc += car;
    acc += opl_chan(5, amv, vi, &car), acc += car;
    if (opl_chip.rhy)
        acc += opl_rhythm(amv, vi);
    else
    {
        int32_t c6, c7, c8;
        acc += opl_chan(6, amv, vi, &c6);
        acc += opl_chan(7, amv, vi, &c7);
        acc += opl_chan(8, amv, vi, &c8);
        opl_chip.held15 = c6;
        opl_chip.held1617 = c7 + c8;
    }

    /* end of sample: counters (spec 1.3); test bit 7 holds them at 0 */
    const uint32_t lfo = opl_chip.lfo;
    {
        uint32_t T = opl_chip.T + (opl_chip.carry | opl_chip.sub);
        opl_chip.carry = (uint8_t)(T >> 18);
        T &= 0x3ffff;
        opl_chip.T = T;
        if (opl_chip.sub)
        {
            opl_chip.eglow = T & 3;
            const uint32_t t = T & 0x1fff;
            opl_chip.egshift = (uint8_t)(t ? opl_ctz(t) + 1 : 0);
        }
        opl_chip.sub ^= 1;
    }
    uint32_t N = opl_chip.N;
    if (test & 0x80)
    {
        opl_chip.tstep = 0;
        opl_chip.vib = 0;
        opl_chip.lfo = 0;
        N = (N << 18) | 0x3ffff;
    }
    else
    {
        opl_chip.tstep = (lfo & 63) == 63;
        opl_chip.vib = (uint8_t)((opl_chip.vib + ((lfo == 1023) | ((test >> 3) & 1))) & 7);
        opl_chip.lfo = (uint16_t)((lfo + 1) & 1023);
        N = ((N << 9) | (((N >> 14) ^ N) & 0x1ff)) & 0x7fffff;
        N = (N << 9) | (((N >> 14) ^ N) & 0x1ff);
    }
    opl_chip.N = N & 0x7fffff;

    /* YM3014B (spec 6.2) */
    const int32_t s = OPL_SSAT(acc, 16);
    uint32_t t = (uint32_t)(s >> 9);
    t = (t ^ (uint32_t)((int32_t)t >> 31)) & 63;
    const uint32_t sh = t ? opl_bits(t) : 0;
    return (int16_t)(int32_t)((uint32_t)(s >> sh) << sh);
}

#pragma GCC pop_options

/* ---------------- reset ---------------- */

/* Every cache from the register shadow. */
static void opl_rebuild(void)
{
    opl_chip.dam = opl_chip.bd >> 7;
    opl_chip.dvb = (opl_chip.bd >> 6) & 1;
    opl_chip.rhy = (opl_chip.bd >> 5) & 1;
    for (int c = 0; c < 9; c++)
    {
        opl_view_k6(&opl_chip.cv[c]);
        opl_view_k6(&opl_chip.mv[c]);
        opl_chan_fb(c);
    }
    for (int i = 0; i < 18; i++)
    {
        opl_slot_t *s = &opl_chip.sl[i];
        s->v = opl_is_mod(i) ? &opl_chip.mv[opl_slotch[i]] : &opl_chip.cv[opl_slotch[i]];
        s->key = (uint8_t)(s->v->kon | (i >= 12 && opl_drum(i)));
        s->ta = (s->r20 & 0x80) ? 0xff0000u : 0;
        s->wav = opl_wav[s->ws];
        opl_slot_pg(s);
        opl_slot_ksl(s);
        opl_slot_eg(s);
        opl_slot_drec(s);
        opl_eg_rec(s);
    }
}

void opl_reset(void)
{
    memset(&opl_chip, 0, sizeof opl_chip);
    memset(&opl_queue, 0, sizeof opl_queue);
    for (int i = 0; i < 18; i++)
    {
        opl_chip.sl[i].level = 0x1ff;
        opl_chip.sl[i].state = OPL_REL;
    }
    opl_chip.tof = 1;
    opl_chip.N = 1;
    opl_chip.credit = 96;
    opl_rebuild();
}

void opl_pace_get(uint32_t *credit, bool *late)
{
    *credit = opl_chip.credit;
    *late = opl_chip.lag & 7;
}

uint32_t opl_take_drops(void)
{
    const uint32_t d = opl_queue.drops;
    opl_queue.drops = 0;
    return d;
}

/* ---------------- the mapping ---------------- */

static uint16_t opl_xaddr = 0xFFFF;

uint16_t opl_xaddr_get(void) { return opl_xaddr; }

void opl_xaddr_set(uint16_t word) { opl_xaddr = word; }

void opl_park(void) { opl_xaddr = 0xFFFF; }

void opl_map(uint16_t word)
{
    opl_reset();
    opl_xaddr = word;
}

/* ---------------- savestate ---------------- */

/* A view as its A0, B0 and C0 bytes. */
static void opl_put_view(sst_cursor_t *c, const opl_view_t *v)
{
    sst_put_u8(c, (uint8_t)v->fnum);
    sst_put_u8(c, (uint8_t)(v->kon << 5 | v->blk << 2 | v->fnum >> 8));
    sst_put_u8(c, (uint8_t)(v->fb << 1 | v->con));
}

static bool opl_get_view(sst_cursor_t *c, opl_view_t *v)
{
    const uint32_t a0 = sst_get_u8(c), b0 = sst_get_u8(c), c0 = sst_get_u8(c);
    v->fnum = (uint16_t)(a0 | (b0 & 3) << 8);
    v->blk = (b0 >> 2) & 7;
    v->kon = (b0 >> 5) & 1;
    v->con = c0 & 1;
    v->fb = (c0 >> 1) & 7;
    return b0 < 64 && c0 < 16;
}

static bool opl_same_view(const opl_view_t *a, const opl_view_t *b, bool fb)
{
    return a->fnum == b->fnum && a->blk == b->blk && a->kon == b->kon &&
           a->con == b->con && (!fb || a->fb == b->fb);
}

static bool opl_out13(uint32_t h)
{
    return (int16_t)h >= -4085 && (int16_t)h <= 4084;
}

void opl_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u16(c, opl_xaddr);
    for (int i = 0; i < 18; i++)
    {
        const opl_slot_t *s = &opl_chip.sl[i];
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
        sst_put_u32(c, opl_chip.ch[i].hist);
        opl_put_view(c, &opl_chip.cv[i]);
        opl_put_view(c, &opl_chip.mv[i]);
    }
    sst_put_u8(c, opl_chip.test);
    sst_put_u8(c, opl_chip.nts);
    sst_put_u8(c, opl_chip.bd);
    sst_put_u8(c, opl_chip.lag);
    sst_put_u8(c, opl_chip.vib);
    sst_put_u16(c, opl_chip.lfo);
    sst_put_u16(c, opl_chip.tv);
    sst_put_u8(c, opl_chip.tdir);
    sst_put_u8(c, opl_chip.tof);
    sst_put_u8(c, opl_chip.tstep);
    sst_put_u32(c, opl_chip.T);
    sst_put_u8(c, opl_chip.carry);
    sst_put_u8(c, opl_chip.sub);
    sst_put_u8(c, opl_chip.egshift);
    sst_put_u8(c, opl_chip.eglow);
    sst_put_u32(c, opl_chip.N);
    sst_put_u8(c, opl_chip.cb);
    sst_put_i32(c, opl_chip.held15);
    sst_put_i32(c, opl_chip.held1617);
    const opl_queue_t *q = &opl_queue;
    sst_put_u16(c, q->count);
    for (uint32_t i = 0; i < OPL_QUEUE_SIZE; i++)
        sst_put_u16(c, i < q->count ? q->entry[(uint8_t)(q->head + i)] : 0);
    sst_put_u8(c, opl_chip.credit);
    sst_put_u32(c, q->drops);
}

bool opl_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    const uint16_t at = sst_get_u16(c);
    memset(&opl_chip, 0, sizeof opl_chip);
    memset(&opl_queue, 0, sizeof opl_queue);
    bool ok = at == 0xFFFF || opl_block_ok(at);
    for (int i = 0; i < 18; i++)
    {
        opl_slot_t *s = &opl_chip.sl[i];
        s->P = sst_get_u32(c);
        s->level = sst_get_u16(c);
        s->state = sst_get_u8(c);
        s->r20 = sst_get_u8(c);
        s->r40 = sst_get_u8(c);
        s->r60 = sst_get_u8(c);
        s->r80 = sst_get_u8(c);
        s->ws = sst_get_u8(c);
        ok &= !(s->P & 0x1fff) && s->level <= 0x1ff && s->state <= OPL_REL && s->ws <= 3;
    }
    for (int i = 0; i < 9; i++)
    {
        opl_chip.ch[i].hist = sst_get_u32(c);
        ok &= opl_out13(opl_chip.ch[i].hist) && opl_out13(opl_chip.ch[i].hist >> 16);
        ok &= opl_get_view(c, &opl_chip.cv[i]);
        ok &= opl_get_view(c, &opl_chip.mv[i]);
    }
    opl_chip.test = sst_get_u8(c);
    opl_chip.nts = sst_get_u8(c);
    opl_chip.bd = sst_get_u8(c);
    opl_chip.lag = sst_get_u8(c);
    opl_chip.vib = sst_get_u8(c);
    opl_chip.lfo = sst_get_u16(c);
    opl_chip.tv = sst_get_u16(c);
    opl_chip.tdir = sst_get_u8(c);
    opl_chip.tof = sst_get_u8(c);
    opl_chip.tstep = sst_get_u8(c);
    opl_chip.T = sst_get_u32(c);
    opl_chip.carry = sst_get_u8(c);
    opl_chip.sub = sst_get_u8(c);
    opl_chip.egshift = sst_get_u8(c);
    opl_chip.eglow = sst_get_u8(c);
    opl_chip.N = sst_get_u32(c);
    opl_chip.cb = sst_get_u8(c);
    opl_chip.held15 = sst_get_i32(c);
    opl_chip.held1617 = sst_get_i32(c);
    ok &= opl_chip.nts <= 1 && opl_chip.lag <= 0x3f && opl_chip.vib <= 7 && opl_chip.lfo <= 1023 && opl_chip.tv <= 511;
    ok &= opl_chip.tdir <= 1 && opl_chip.tof <= 1 && opl_chip.tstep <= 1;
    ok &= opl_chip.T <= 0x3ffff && opl_chip.carry <= 1 && opl_chip.sub <= 1 && opl_chip.egshift <= 13 && opl_chip.eglow <= 3;
    ok &= opl_chip.N && opl_chip.N <= 0x7fffff && !(opl_chip.cb & ~0x28);
    ok &= opl_chip.held15 >= -8170 && opl_chip.held15 <= 8168 && opl_chip.held1617 >= -8170 && opl_chip.held1617 <= 8168;
    /* the modulator view differs from the carrier's only while it lags */
    for (int i = 0; i < 9; i++)
        ok &= opl_same_view(&opl_chip.mv[i], &opl_chip.cv[i], !(opl_chip.lag & (1u << i))) ||
              (i < 3 && (opl_chip.lag & (1u << i)));
    opl_queue_t *q = &opl_queue;
    q->count = sst_get_u16(c);
    ok &= q->count <= OPL_QUEUE_SIZE;
    for (uint32_t i = 0; i < OPL_QUEUE_SIZE; i++)
    {
        const uint32_t e = sst_get_u16(c);
        q->entry[i] = (uint16_t)e;
        ok &= i >= q->count || ((opl_regs[(e & 0xff) >> 3] >> (e & 7)) & 1);
    }
    opl_chip.credit = sst_get_u8(c);
    q->drops = sst_get_u32(c);
    ok &= opl_chip.credit >= 72 && opl_chip.credit <= (q->count ? 192 : 96) && !(opl_chip.credit % 24);
    ok &= q->drops <= 0x7FFFFFFF;
    if (!ok || !sst_ok(c))
    {
        opl_reset();
        return false;
    }
    opl_rebuild();
    opl_xaddr = at;
    return true;
}
