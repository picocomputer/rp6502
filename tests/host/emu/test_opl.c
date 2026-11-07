/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The YM3812 engine against YM3812-LLE, nukeykt's gate-level model of the
 * die, sample by sample; its write pacing against a reference written from
 * the rule; its audio on bursts against the spec model; and its savestate. */

#include "core/aud/opl.h"
#include "opl_lle.h"
#include "opl_model.h"
#include "opl_script.h"

#include "utest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(OPL_QUEUE_SIZE == 256, "the pacing reference holds 256 writes");

#define ASSERT_OK(x)                                \
    do                                              \
    {                                               \
        const char *why_ = (x);                     \
        ASSERT_TRUE_MSG(!why_, why_ ? why_ : "");   \
    } while (0)

/* The writes the engine applied, each with the sample it was applied
 * before, from its trace hook. */
static opl_script_t applied;
static uint32_t now;

void opl_trace(uint8_t reg, uint8_t val)
{
    opl_script_add(&applied, now, reg, val);
}

/* Slots that re-evaluate the whole rule in the next sample. */
int opl_forced_slots(void);

static char why[640];

/* Fed as the firmware feeds it: a sample's writes are pushed, then the
 * sample is rendered. */
static void engine_run(const opl_script_t *pushes, int16_t *out)
{
    opl_reset();
    applied.count = 0;
    size_t i = 0;
    for (now = 0; now < pushes->samples; now++)
    {
        for (; i < pushes->count && pushes->w[i].n == now; i++)
            opl_push(pushes->w[i].reg, pushes->w[i].val);
        out[now] = opl_sample();
    }
}

static bool save(void);
static bool load(size_t len);

/* NULL when no slot is left between samples to re-evaluate the whole rule,
 * straight and with the state saved and loaded before every sample, else the
 * first sample after which one is. */
static const char *none_forced(const opl_script_t *pushes)
{
    for (int cut = 0; cut < 2; cut++)
    {
        opl_reset();
        applied.count = 0;
        size_t i = 0;
        for (now = 0; now < pushes->samples; now++)
        {
            for (; i < pushes->count && pushes->w[i].n == now; i++)
                opl_push(pushes->w[i].reg, pushes->w[i].val);
            if (cut && !(save() && load(OPL_SST_SIZE)))
            {
                snprintf(why, sizeof why, "sample %u: the load refused a saved state", (unsigned)now);
                return why;
            }
            opl_sample();
            const int k = opl_forced_slots();
            if (k)
            {
                snprintf(why, sizeof why, "after sample %u%s, %d slots re-evaluate in full", (unsigned)now,
                         cut ? " loaded" : "", k);
                return why;
            }
        }
    }
    return NULL;
}

static void model_run(const opl_script_t *schedule, int16_t *out)
{
    model_reset();
    size_t i = 0;
    for (uint32_t n = 0; n < schedule->samples; n++)
    {
        for (; i < schedule->count && schedule->w[i].n == n; i++)
            model_write(schedule->w[i].reg, schedule->w[i].val);
        out[n] = model_sample();
    }
}

/* NULL when a and b agree, else the first sample they differ at and the
 * last writes of the script up to it. */
static const char *differ(const char *an, const int16_t *a, const char *bn, const int16_t *b,
                          const opl_script_t *schedule)
{
    for (uint32_t n = 0; n < schedule->samples; n++)
    {
        if (a[n] == b[n])
            continue;
        int len = snprintf(why, sizeof why, "sample %u: %s %d, %s %d; writes up to it:",
                           (unsigned)n, an, a[n], bn, b[n]);
        size_t last = 0;
        while (last < schedule->count && schedule->w[last].n <= n)
            last++;
        for (size_t i = last > 8 ? last - 8 : 0; i < last && len < (int)sizeof why - 24; i++)
            len += snprintf(why + len, sizeof why - (size_t)len, " %u:%02X=%02X",
                            (unsigned)schedule->w[i].n, schedule->w[i].reg, schedule->w[i].val);
        return why;
    }
    return NULL;
}

/* NULL when the engine applied what the reference schedules, where it does. */
static const char *same_schedule(const opl_script_t *ref)
{
    for (size_t i = 0; i < applied.count || i < ref->count; i++)
    {
        const opl_write_t *e = i < applied.count ? &applied.w[i] : NULL;
        const opl_write_t *w = i < ref->count ? &ref->w[i] : NULL;
        if (e && w && e->n == w->n && e->reg == w->reg && e->val == w->val)
            continue;
        char es[32] = "nothing", ws[32] = "nothing";
        if (e)
            snprintf(es, sizeof es, "%02X=%02X at %u", e->reg, e->val, (unsigned)e->n);
        if (w)
            snprintf(ws, sizeof ws, "%02X=%02X at %u", w->reg, w->val, (unsigned)w->n);
        snprintf(why, sizeof why, "write %zu: the engine applied %s, the rule %s", i, es, ws);
        return why;
    }
    return NULL;
}

/* The engine against LLE sample for sample, and the model too, with the
 * engine's schedule checked against the reference's. LLE takes the paced
 * schedule, or with bus the pushes as they stand, skipped offsets and all;
 * every other write must then apply as it is pushed. */
static const char *versus_lle(const opl_script_t *pushes, bool bus)
{
    uint32_t *at = malloc(pushes->count * sizeof *at + 1);
    int16_t *die = malloc(pushes->samples * sizeof *die);
    int16_t *got = malloc(pushes->samples * sizeof *got);
    opl_script_t schedule = {0};
    opl_pace(pushes, at, &schedule);
    const char *r = NULL;
    for (size_t i = 0; bus && i < pushes->count && !r; i++)
        if (at[i] != OPL_SKIPPED && at[i] != pushes->w[i].n)
        {
            snprintf(why, sizeof why, "write %zu (%02X=%02X at %u) is not applied as pushed", i,
                     pushes->w[i].reg, pushes->w[i].val, (unsigned)pushes->w[i].n);
            r = why;
        }
    if (!r)
        r = lle_run(bus ? pushes : &schedule, 5, die);
    if (!r)
    {
        engine_run(pushes, got);
        r = same_schedule(&schedule);
    }
    if (!r)
        r = differ("engine", got, "LLE", die, &schedule);
    if (!r)
    {
        model_run(&schedule, got);
        r = differ("model", got, "LLE", die, &schedule);
    }
    opl_script_free(&schedule);
    free(at);
    free(die);
    free(got);
    return r;
}

/* On bursts the die cannot take: the engine's schedule and drops against the
 * reference's, and its audio against the model's on that schedule. */
static const char *versus_model(const opl_script_t *pushes)
{
    uint32_t *at = malloc(pushes->count * sizeof *at + 1);
    int16_t *want = malloc(pushes->samples * sizeof *want);
    int16_t *got = malloc(pushes->samples * sizeof *got);
    opl_script_t schedule = {0};
    uint32_t drops = opl_pace(pushes, at, &schedule);
    engine_run(pushes, got);
    const char *r = same_schedule(&schedule);
    uint32_t took = opl_take_drops();
    if (!r && (took != drops || opl_take_drops() != 0))
    {
        snprintf(why, sizeof why, "engine dropped %u, the rule %u", (unsigned)took, (unsigned)drops);
        r = why;
    }
    if (!r)
    {
        model_run(&schedule, want);
        r = differ("engine", got, "model", want, &schedule);
    }
    opl_script_free(&schedule);
    free(at);
    free(want);
    free(got);
    return r;
}

/* versus_lle, then frees the script. */
static const char *lle_free(opl_script_t *s)
{
    const char *r = versus_lle(s, false);
    opl_script_free(s);
    return r;
}

static const char *lle_random(uint32_t seed, uint32_t samples, unsigned flags)
{
    opl_script_t s = {0};
    opl_script_random(&s, seed, samples, flags);
    return lle_free(&s);
}

static const char *lle_dense(uint32_t seed, uint32_t samples, unsigned flags)
{
    opl_script_t s = {0};
    opl_script_dense(&s, seed, samples, flags);
    return lle_free(&s);
}

/* ---------------- the harness ---------------- */

/* A stopped program's writes go with it. */
UTEST(opl, a_park_drops_the_writes_still_queued)
{
    opl_map(0x4000);
    for (int k = 0; k < 10; k++)
        opl_push(0x40, (uint8_t)k);
    ASSERT_EQ(RING_LEN(&opl_queue.ring), 10u);
    opl_park();
    ASSERT_EQ(RING_LEN(&opl_queue.ring), 0u);
}

/* The test-register preamble leaves the die in one state however long IC
 * was held, and so whatever its counters were. A random script applies each
 * write as it is pushed, so it is its own schedule. */
UTEST(opl, lle_preamble_hides_power_on)
{
    opl_script_t s = {0};
    opl_script_random(&s, 41, 4000, OPL_RHYTHM);
    int16_t *a = malloc(s.samples * sizeof *a), *b = malloc(s.samples * sizeof *b);
    ASSERT_OK(lle_run(&s, 5, a));
    ASSERT_OK(lle_run(&s, 13, b));
    const char *r = differ("IC 5", a, "IC 13", b, &s);
    free(a);
    free(b);
    opl_script_free(&s);
    ASSERT_OK(r);
}

/* ---------------- against the die, paced as the chip allows ---------------- */

UTEST(opl, lle_power_on)
{
    opl_script_t s = {0};
    opl_script_power_on(&s);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_envelopes)
{
    opl_script_t s = {0};
    opl_script_envelopes(&s);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_levels)
{
    opl_script_t s = {0};
    opl_script_levels(&s);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_waves)
{
    opl_script_t s = {0};
    opl_script_waves(&s);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_feedback)
{
    opl_script_t s = {0};
    opl_script_feedback(&s);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_lfo)
{
    opl_script_t s = {0};
    opl_script_lfo(&s);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_rhythm)
{
    opl_script_t s = {0};
    opl_script_rhythm(&s);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_retrigger)
{
    opl_script_t s = {0};
    opl_script_retrigger(&s);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_clip)
{
    opl_script_t s = {0};
    opl_script_clip(&s, false);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_clip_rhythm)
{
    opl_script_t s = {0};
    opl_script_clip(&s, true);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_random) { ASSERT_OK(lle_random(1, 8000, 0)); }
UTEST(opl, lle_random_rhythm) { ASSERT_OK(lle_random(2, 8000, OPL_RHYTHM)); }
UTEST(opl, lle_random_alias) { ASSERT_OK(lle_random(3, 8000, OPL_RHYTHM | OPL_ALIAS)); }
UTEST(opl, lle_random_slow) { ASSERT_OK(lle_random(4, 8000, OPL_RHYTHM | OPL_SLOW_ENV)); }
UTEST(opl, lle_dense) { ASSERT_OK(lle_dense(5, 8000, OPL_RHYTHM)); }
UTEST(opl, lle_dense_alias) { ASSERT_OK(lle_dense(6, 8000, OPL_RHYTHM | OPL_ALIAS)); }
UTEST(opl, lle_dense_slow) { ASSERT_OK(lle_dense(7, 8000, OPL_SLOW_ENV | OPL_GAP2)); }
UTEST(opl, lle_dense_test_bits) { ASSERT_OK(lle_dense(9, 8000, OPL_RHYTHM | OPL_TEST_BITS)); }
UTEST(opl, lle_test_bits_1)
{
    opl_script_t s = {0};
    opl_script_test_bits(&s, 1);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_test_bits_2)
{
    opl_script_t s = {0};
    opl_script_test_bits(&s, 2);
    ASSERT_OK(lle_free(&s));
}

UTEST(opl, lle_test_bits_random) { ASSERT_OK(lle_random(8, 8000, OPL_RHYTHM | OPL_TEST_BITS)); }

/* Every offset the engine skips, on the die's bus between paced writes: the
 * die must not hear them either. */
UTEST(opl, lle_holes)
{
    opl_script_t src = {0}, s = {0};
    opl_script_random(&src, 10, 8000, OPL_RHYTHM | OPL_ALIAS);
    opl_script_holes(&s, &src, 10);
    opl_script_free(&src);
    bool seen[256] = {false};
    for (size_t i = 0; i < s.count; i++)
        seen[s.w[i].reg] = true;
    int missing = 0;
    for (int reg = 0; reg < 256; reg++)
        missing += !opl_is_reg((uint8_t)reg) && !seen[reg];
    const char *r = versus_lle(&s, true);
    opl_script_free(&s);
    ASSERT_EQ(missing, 0);
    ASSERT_OK(r);
}

/* versus_lle and none_forced, then frees the script. */
static const char *lle_settled_free(opl_script_t *s)
{
    const char *r = versus_lle(s, false);
    if (!r)
        r = none_forced(s);
    opl_script_free(s);
    return r;
}

/* Key-offs in attack at level 0x1FF, by each source: in the mix that sample,
 * muted from the next. */
UTEST(opl, lle_keyoff_silent)
{
    opl_script_t s = {0};
    opl_script_keyoff_silent(&s);
    ASSERT_OK(lle_settled_free(&s));
}

/* Attack to decay with SL 0, inline and after a forced evaluation: one
 * decay sample, then sustain. */
UTEST(opl, lle_decay_sl0)
{
    opl_script_t s = {0};
    opl_script_decay_sl0(&s);
    ASSERT_OK(lle_settled_free(&s));
}

/* About 11 s, across the envelope timer's wrap. */
UTEST(opl, lle_eg_wrap)
{
    opl_script_t s = {0};
    opl_script_eg_wrap(&s);
    ASSERT_OK(lle_free(&s));
}

/* ---------------- pacing ---------------- */

static uint32_t engine_credit(void);

/* The rule's consequences, worked out by hand, from the reference and from
 * the engine. */
UTEST(opl, pacing_rule)
{
    opl_script_t s = {0}, schedule = {0};
    uint32_t at[1200];
    s.samples = 2000;
    /* Two samples apart: each at once. */
    for (uint32_t n = 10; n < 30; n += 2)
        opl_script_add(&s, n, 0x40, (uint8_t)n);
    /* Two at 40, then one at 43 and one at 44: at 40, 42, 43 and 45, as the
     * credit stops at 96 once the second has gone. */
    const size_t capped = s.count;
    static const uint32_t capped_at[4] = {40, 42, 43, 45};
    opl_script_add(&s, 40, 0x43, 1);
    opl_script_add(&s, 40, 0x43, 2);
    opl_script_add(&s, 43, 0x43, 3);
    opl_script_add(&s, 44, 0x43, 4);
    /* 256 at once from rest: write k at ceil(4k/3), none dropped. */
    const size_t rest = s.count;
    for (int k = 0; k < 256; k++)
        opl_script_add(&s, 100, 0x40, (uint8_t)k);
    /* 300 holes and timer writes never queue; then 256 queue and 1 drops. */
    const size_t holes = s.count;
    for (int k = 0; k < 300; k++)
        opl_script_add(&s, 500, (uint8_t)(k % 2 ? 0x02 : 0x16), 0);
    const size_t full = s.count;
    for (int k = 0; k < 257; k++)
        opl_script_add(&s, 500, 0x41, (uint8_t)k);
    /* One a sample: 3 of every 4 samples apply one. */
    const size_t steady = s.count;
    for (uint32_t n = 1000; n < 1100; n++)
        opl_script_add(&s, n, 0x42, (uint8_t)n);
    /* Five at once from rest, A0 third: its late half takes sample 4, so the
     * operator write behind it moves from 4 to 5 and the last is still at 6. */
    const size_t moved = s.count;
    static const uint32_t moved_at[5] = {0, 2, 3, 5, 6};
    for (int k = 0; k < 5; k++)
        opl_script_add(&s, 1200, (uint8_t)(k == 2 ? 0xA0 : 0x41), (uint8_t)k);
    /* Seven at once from rest at 0, 2, 3, 4, 6, 7 and 8; with B0 second, its
     * late half takes sample 3 and the rest apply at 4, 5, 6, 7 and 8. */
    const size_t seven = s.count;
    static const uint32_t seven_at[2][7] = {{0, 2, 3, 4, 6, 7, 8}, {0, 2, 4, 5, 6, 7, 8}};
    for (int k = 0; k < 7; k++)
        opl_script_add(&s, 1300, 0x42, (uint8_t)k);
    for (int k = 0; k < 7; k++)
        opl_script_add(&s, 1400, (uint8_t)(k == 1 ? 0xB0 : 0x42), (uint8_t)k);
    /* Channel 0-2 writes alone, every form, at once: one per two samples while
     * the credit climbs to 192. The operator writes behind them apply 5 in a
     * row on it, then 3 in every 4. */
    static const uint8_t ch02[15] = {0xA0, 0xA1, 0xA2, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2,
                                     0xBE, 0xBF, 0xC0, 0xC1, 0xC2, 0xCE, 0xCF};
    static const uint32_t after_at[8] = {30, 31, 32, 33, 34, 36, 37, 38};
    const size_t run = s.count;
    for (int k = 0; k < 15; k++)
        opl_script_add(&s, 1500, ch02[k], (uint8_t)k);
    for (int k = 0; k < 8; k++)
        opl_script_add(&s, 1500, 0x42, (uint8_t)k);
    /* The aliases and C3-C5 hold nothing back: from rest, with an operator
     * write every third, each applies just before a sample that applies one. */
    static const uint8_t unheld_regs[12] = {0xA9, 0xAA, 0xAB, 0xB9, 0xBA, 0xBB,
                                            0xC9, 0xCA, 0xCB, 0xC3, 0xC4, 0xC5};
    const size_t unheld = s.count;
    for (int k = 0; k < 19; k++)
        opl_script_add(&s, 1600, k % 3 ? unheld_regs[k - k / 3 - 1] : 0x42, (uint8_t)k);
    ASSERT_LE(s.count, sizeof at / sizeof *at);

    ASSERT_EQ(opl_pace(&s, at, &schedule), 1u);
    for (size_t k = 0; k < capped; k++)
        ASSERT_EQ(at[k], s.w[k].n);
    for (size_t k = 0; k < 4; k++)
        ASSERT_EQ(at[capped + k], capped_at[k]);
    for (uint32_t k = 0; k < 256; k++)
        ASSERT_EQ(at[rest + k], 100 + (4 * k + 2) / 3);
    for (size_t k = 0; k < 300; k++)
        ASSERT_EQ(at[holes + k], OPL_SKIPPED);
    for (uint32_t k = 0; k < 256; k++)
        ASSERT_EQ(at[full + k], 500 + (4 * k + 2) / 3);
    ASSERT_EQ(at[full + 256], OPL_DROPPED);
    for (uint32_t k = 0; k < 100; k++)
        ASSERT_EQ(at[steady + k], 1000 + (4 * k + 2) / 3);
    for (size_t k = 0; k < 5; k++)
        ASSERT_EQ(at[moved + k], 1200 + moved_at[k]);
    for (size_t k = 0; k < 7; k++)
    {
        ASSERT_EQ(at[seven + k], 1300 + seven_at[0][k]);
        ASSERT_EQ(at[seven + 7 + k], 1400 + seven_at[1][k]);
    }
    for (uint32_t k = 0; k < 15; k++)
        ASSERT_EQ(at[run + k], 1500 + 2 * k);
    for (size_t k = 0; k < 8; k++)
        ASSERT_EQ(at[run + 15 + k], 1500 + after_at[k]);
    for (uint32_t k = 0; k < 19; k++)
        ASSERT_EQ(at[unheld + k], 1600 + (4 * k + 2) / 3);

    int16_t *out = malloc(s.samples * sizeof *out);
    engine_run(&s, out);
    free(out);
    const char *r = same_schedule(&schedule);
    const uint32_t drops = opl_take_drops(), again = opl_take_drops();
    /* Again, reading the engine's credit after every sample: it reaches 192
     * and never passes it. */
    uint32_t top = 0;
    opl_reset();
    for (size_t n = 0, i = 0; n < s.samples; n++)
    {
        for (; i < s.count && s.w[i].n == n; i++)
            opl_push(s.w[i].reg, s.w[i].val);
        opl_sample();
        const uint32_t c = engine_credit();
        top = c > top ? c : top;
    }
    opl_script_free(&s);
    opl_script_free(&schedule);
    ASSERT_OK(r);
    ASSERT_EQ(drops, 1u);
    ASSERT_EQ(again, 0u);
    ASSERT_EQ(top, 192u);
}

/* ---------------- against the model, in bursts ---------------- */

static const char *bursts(uint32_t seed, unsigned flags)
{
    opl_script_t s = {0};
    opl_script_bursts(&s, seed, 30000, flags);
    const char *r = versus_model(&s);
    opl_script_free(&s);
    return r;
}

UTEST(opl, model_bursts_1) { ASSERT_OK(bursts(1, 0)); }
UTEST(opl, model_bursts_2) { ASSERT_OK(bursts(2, 0)); }
UTEST(opl, model_bursts_3) { ASSERT_OK(bursts(3, 0)); }
UTEST(opl, model_bursts_test_bits) { ASSERT_OK(bursts(4, OPL_TEST_BITS)); }

UTEST(opl, model_flood)
{
    opl_script_t s = {0};
    opl_script_flood(&s, 1);
    const char *r = versus_model(&s);
    opl_script_free(&s);
    ASSERT_OK(r);
}

/* ---------------- savestate ---------------- */

static uint8_t blob[OPL_SST_SIZE + 16];

static bool save(void)
{
    sst_cursor_t c = {blob, blob + sizeof blob, false};
    opl_sst_save(&c, 0);
    return sst_ok(&c) && c.at == blob + OPL_SST_SIZE;
}

static bool load(size_t len)
{
    sst_cursor_t c = {blob, blob + len, false};
    return opl_sst_load(&c, 0) && sst_ok(&c) && c.at == blob + OPL_SST_SIZE;
}

/* Where fields sit in a state, in opl_sst_save's order: the pointer at 0;
 * per slot P at 0, level 4, state 6 and WS 11; per channel the feedback
 * history at 0 and the carrier and modulator views at 4 and 7, each its A0,
 * B0 and C0 bytes. */
#define SST_SLOT(i, at) (2 + 12 * (i) + (at))
#define SST_CH(i, at) (2 + 18 * 12 + 10 * (i) + (at))
#define SST_GLOB (2 + 18 * 12 + 9 * 10)
#define SST_T (SST_GLOB + 12)
#define SST_SUB (SST_GLOB + 17)
#define SST_QUEUE (OPL_SST_SIZE - (2 + 2 * OPL_QUEUE_SIZE + 1 + 4))
#define SST_CREDIT (OPL_SST_SIZE - 5)
#define SST_DROPS (OPL_SST_SIZE - 4)

/* The engine's credit, from a saved state. */
static uint32_t engine_credit(void)
{
    save();
    return blob[SST_CREDIT];
}

static void put_be(uint8_t *p, unsigned len, uint32_t v)
{
    for (unsigned k = 0; k < len; k++)
        p[k] = (uint8_t)(v >> 8 * (len - 1 - k));
}

/* The engine from the state in start, or from reset. */
static void begin(const uint8_t *start)
{
    opl_reset();
    if (start)
    {
        memcpy(blob, start, OPL_SST_SIZE);
        load(OPL_SST_SIZE);
    }
}

/* The script run straight from begin, then again saved, reset and loaded
 * between every sample's pushes and its render. Counts the cuts where the
 * queue was part full and where it was full. */
static const char *round_trip(const opl_script_t *pushes, const uint8_t *start, int *part, int *full)
{
    int16_t *out[2] = {malloc(pushes->samples * sizeof **out), malloc(pushes->samples * sizeof **out)};
    uint32_t drops[2] = {0, 0};
    const char *r = NULL;
    *part = *full = 0;
    for (int cut = 0; cut < 2 && !r; cut++)
    {
        begin(start);
        size_t i = 0;
        for (uint32_t n = 0; n < pushes->samples && !r; n++)
        {
            for (; i < pushes->count && pushes->w[i].n == n; i++)
                opl_push(pushes->w[i].reg, pushes->w[i].val);
            if (cut)
            {
                const uint32_t q = RING_LEN(&opl_queue.ring);
                *part += q > 1 && q < OPL_QUEUE_SIZE;
                *full += q == OPL_QUEUE_SIZE;
                const char *fail = save() ? NULL : "the save did not fill OPL_SST_SIZE";
                opl_reset();
                if (!fail && !load(OPL_SST_SIZE))
                    fail = "the load refused a saved state";
                if (fail)
                {
                    snprintf(why, sizeof why, "sample %u: %s", (unsigned)n, fail);
                    r = why;
                }
            }
            out[cut][n] = opl_sample();
        }
        drops[cut] = opl_take_drops();
    }
    if (!r)
        r = differ("straight", out[0], "reloaded", out[1], pushes);
    if (!r && drops[0] != drops[1])
    {
        snprintf(why, sizeof why, "straight dropped %u, reloaded %u", (unsigned)drops[0], (unsigned)drops[1]);
        r = why;
    }
    free(out[0]);
    free(out[1]);
    return r;
}

UTEST(opl, sst_round_trip_bursts)
{
    opl_script_t s = {0};
    int part, full;
    opl_script_bursts(&s, 5, 20000, OPL_TEST_BITS);
    const char *r = round_trip(&s, NULL, &part, &full);
    opl_script_free(&s);
    ASSERT_OK(r);
    ASSERT_GT(part, 0);
}

UTEST(opl, sst_round_trip_flood)
{
    opl_script_t s = {0};
    int part, full;
    opl_script_flood(&s, 2);
    const char *r = round_trip(&s, NULL, &part, &full);
    opl_script_free(&s);
    ASSERT_OK(r);
    ASSERT_GT(full, 0);
}

/* Across the envelope timer's wrap, whose carry lives for one sample. */
UTEST(opl, sst_round_trip_eg_wrap)
{
    static uint8_t start[OPL_SST_SIZE];
    opl_script_t s = {0}, tail = {0};
    opl_script_bursts(&s, 7, 6000, 0);
    s.samples = 2000;
    int16_t *out = malloc(s.samples * sizeof *out);
    engine_run(&s, out);
    free(out);
    ASSERT_TRUE(save());
    memcpy(start, blob, sizeof start);
    put_be(start + SST_T, 4, 0x3FFFF);
    start[SST_SUB] = 1;
    tail.samples = 4000;
    for (size_t i = 0; i < s.count; i++)
        if (s.w[i].n >= s.samples)
            opl_script_add(&tail, s.w[i].n - s.samples, s.w[i].reg, s.w[i].val);
    int part, full;
    const char *r = round_trip(&tail, start, &part, &full);
    opl_script_free(&s);
    opl_script_free(&tail);
    ASSERT_OK(r);
}

/* A state saved with writes queued, for the load checks. */
static void save_busy(void)
{
    opl_script_t s = {0};
    opl_script_bursts(&s, 6, 2000, 0);
    int16_t *out = malloc(s.samples * sizeof *out);
    engine_run(&s, out);
    for (int k = 0; k < 40; k++)
        opl_push((uint8_t)(0x40 + k % 6), (uint8_t)k);
    free(out);
    opl_script_free(&s);
    save();
}

/* NULL when the engine renders as a reset one does. */
static const char *is_reset(void)
{
    int16_t a[600], b[600];
    opl_script_t s = {0};
    opl_script_power_on(&s);
    s.samples = 600;
    size_t i = 0;
    for (uint32_t n = 0; n < s.samples; n++)
    {
        for (; i < s.count && s.w[i].n == n; i++)
            opl_push(s.w[i].reg, s.w[i].val);
        b[n] = opl_sample();
    }
    uint32_t drops = opl_take_drops();
    engine_run(&s, a);
    const char *r = differ("reset", a, "refused", b, &s);
    if (!r && drops)
        r = "a refused load kept a drop count";
    opl_script_free(&s);
    return r;
}

/* NULL when the load refuses blob and leaves the engine reset, else what. */
static const char *refuses(const char *what)
{
    opl_reset();
    return load(OPL_SST_SIZE) ? what : is_reset();
}

/* Each field the load checks, out of its range alone. */
UTEST(opl, sst_refuses_out_of_range)
{
    static uint8_t good[OPL_SST_SIZE];
    save_busy();
    memcpy(good, blob, sizeof good);
    ASSERT_GT((blob[SST_QUEUE] << 8) | blob[SST_QUEUE + 1], 0);
    ASSERT_TRUE(load(OPL_SST_SIZE));

    static const struct
    {
        size_t at;
        uint8_t len;
        bool flip; /* val is XORed into the field */
        uint32_t val;
        const char *what;
    } bad[] = {
        {0, 2, false, 0xFF01, "a block past the end of XRAM"},
        {SST_SLOT(3, 0), 4, true, 1, "a phase with bits below its 19"},
        {SST_SLOT(3, 4), 2, false, 0x200, "a 10-bit level"},
        {SST_SLOT(3, 6), 1, false, 4, "a fifth envelope state"},
        {SST_SLOT(3, 11), 1, false, 4, "a fifth waveform"},
        {SST_CH(2, 0), 4, false, 4085, "an operator output past 13 bits"},
        {SST_CH(2, 0), 4, false, 0xF00A0000, "an operator output below 13 bits"},
        {SST_CH(2, 5), 1, true, 0x40, "a B0 byte with bit 6"},
        {SST_CH(2, 6), 1, true, 0x10, "a C0 byte with bit 4"},
        {SST_CH(2, 9), 1, true, 0x10, "a modulator C0 byte with bit 4"},
        {SST_CH(7, 7), 1, true, 1, "a channel 6-8 modulator view off its carrier's"},
        {SST_CH(7, 9), 1, true, 2, "a channel 6-8 modulator FB off its carrier's"},
        {SST_GLOB + 1, 1, false, 2, "NTS"},
        {SST_GLOB + 3, 1, false, 0x40, "a lag on channel 6"},
        {SST_GLOB + 4, 1, false, 8, "a vibrato position"},
        {SST_GLOB + 5, 2, false, 1024, "an LFO count"},
        {SST_GLOB + 7, 2, false, 512, "a tremolo count"},
        {SST_GLOB + 9, 1, false, 2, "a tremolo direction"},
        {SST_GLOB + 10, 1, false, 2, "a tremolo edge"},
        {SST_GLOB + 11, 1, false, 2, "a tremolo step"},
        {SST_T, 4, false, 0x40000, "a 19-bit envelope timer"},
        {SST_GLOB + 16, 1, false, 2, "a timer carry"},
        {SST_SUB, 1, false, 2, "a timer parity"},
        {SST_GLOB + 18, 1, false, 14, "a timer shift"},
        {SST_GLOB + 19, 1, false, 4, "timer low bits"},
        {SST_GLOB + 20, 4, false, 0, "a stuck noise register"},
        {SST_GLOB + 20, 4, false, 0x800000, "a 24-bit noise register"},
        {SST_GLOB + 24, 1, false, 1, "a cymbal bit other than 3 and 5"},
        {SST_GLOB + 25, 4, false, 8169, "a held carrier past 2 x 13 bits"},
        {SST_GLOB + 29, 4, false, (uint32_t)-8171, "held carriers below 2 x 13 bits"},
        {SST_QUEUE + 2, 2, false, 0x2000, "a write to offset 00"},
        {SST_QUEUE + 2, 2, false, 0x2016, "a write to offset 16"},
        {SST_QUEUE + 2, 2, false, 0x2002, "a write to timer 1"},
        {SST_CREDIT, 1, false, 25, "a credit that is not a multiple of 24"},
        {SST_CREDIT, 1, false, 48, "a credit below 72"},
        {SST_CREDIT, 1, false, 216, "a credit past 192"},
        {SST_DROPS, 4, false, 0x80000000, "more drops than the count holds"},
    };
    for (size_t k = 0; k < sizeof bad / sizeof *bad; k++)
    {
        memcpy(blob, good, sizeof good);
        uint32_t v = bad[k].val;
        if (bad[k].flip)
            for (unsigned j = 0; j < bad[k].len; j++)
                v ^= (uint32_t)blob[bad[k].at + j] << 8 * (bad[k].len - 1 - j);
        put_be(blob + bad[k].at, bad[k].len, v);
        ASSERT_OK(refuses(bad[k].what));
    }

    /* A channel 3-5 modulator lags its carrier only in FB. */
    memcpy(blob, good, sizeof good);
    blob[SST_GLOB + 3] = 0x10;
    blob[SST_CH(4, 7)] ^= 1;
    ASSERT_OK(refuses("a channel 3-5 modulator F-number off its carrier's"));

    memcpy(blob, good, sizeof good);
    put_be(blob + SST_QUEUE, 2, 0);
    blob[SST_CREDIT] = 120;
    ASSERT_OK(refuses("a credit past 96 with nothing waiting"));

    memcpy(blob, good, sizeof good);
    for (size_t k = 0; k < OPL_QUEUE_SIZE; k++)
        put_be(blob + SST_QUEUE + 2 + 2 * k, 2, 0x2040);
    put_be(blob + SST_QUEUE, 2, OPL_QUEUE_SIZE + 1);
    ASSERT_OK(refuses("more writes than the queue holds"));

    /* A block may start anywhere it ends inside XRAM. */
    for (unsigned at = 0x4001; at <= 0xFF00; at += 0xBEFF)
    {
        memcpy(blob, good, sizeof good);
        put_be(blob, 2, at);
        opl_reset();
        EXPECT_TRUE_MSG(load(OPL_SST_SIZE), "a block that fits refused");
        ASSERT_EQ(opl_xaddr_get(), (uint16_t)at);
    }

    memcpy(blob, good, sizeof good);
    opl_reset();
    EXPECT_FALSE_MSG(load(OPL_SST_SIZE - 1), "a short state loaded");
    ASSERT_OK(is_reset());
}

/* Any byte of a state changed: the load refuses it, or the engine runs from
 * it and saves a state that loads again. */
UTEST(opl, sst_any_byte)
{
    static uint8_t good[OPL_SST_SIZE];
    static const uint8_t vals[] = {0x00, 0x01, 0x7F, 0x80, 0xFF};
    save_busy();
    memcpy(good, blob, sizeof good);
    for (size_t at = 0; at < OPL_SST_SIZE; at++)
        for (size_t v = 0; v < sizeof vals; v++)
        {
            memcpy(blob, good, sizeof good);
            if (blob[at] == vals[v])
                continue;
            blob[at] = vals[v];
            opl_reset();
            if (!load(OPL_SST_SIZE))
                continue;
            for (int n = 0; n < 48; n++)
                opl_sample();
            ASSERT_TRUE(save());
            opl_reset();
            ASSERT_TRUE_MSG(load(OPL_SST_SIZE), "a state the engine saved did not load");
        }
}

UTEST_MAIN()
