/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "core/aud/sine.h"
#include <string.h>

#define BEL_QUEUE_SIZE 8

#define BEL_PEAK 32767
#define BEL_RAIL (-32767)

enum bel_adsr_state
{
    release,
    attack,
    decay,
    sustain,
};

static const uint32_t bel_vol_table[] = {
    256 << 16,
    204 << 16,
    168 << 16,
    142 << 16,
    120 << 16,
    102 << 16,
    86 << 16,
    73 << 16,
    61 << 16,
    50 << 16,
    40 << 16,
    31 << 16,
    22 << 16,
    14 << 16,
    7 << 16,
    0 << 16,
};

/* As psg.c: the 6581 SID's rates in milliseconds, as the increments they
 * come to at AUD_NATIVE_RATE, the rate multiplying before it divides by
 * 1000 because AUD_NATIVE_RATE is not a whole number of kilohertz. */
#define BEL_STEP(ms) ((1 << 24) / (uint32_t)(((uint64_t)AUD_NATIVE_RATE * (ms)) / 1000))

static const uint32_t bel_attack_table[16] = {
    BEL_STEP(2), BEL_STEP(8), BEL_STEP(16), BEL_STEP(24),
    BEL_STEP(38), BEL_STEP(56), BEL_STEP(68), BEL_STEP(80),
    BEL_STEP(100), BEL_STEP(250), BEL_STEP(500), BEL_STEP(800),
    BEL_STEP(1000), BEL_STEP(3000), BEL_STEP(5000), BEL_STEP(8000),
};

static const uint32_t bel_decay_release_table[16] = {
    BEL_STEP(6), BEL_STEP(24), BEL_STEP(48), BEL_STEP(72),
    BEL_STEP(114), BEL_STEP(168), BEL_STEP(204), BEL_STEP(240),
    BEL_STEP(300), BEL_STEP(750), BEL_STEP(1500), BEL_STEP(2400),
    BEL_STEP(3000), BEL_STEP(9000), BEL_STEP(15000), BEL_STEP(24000),
};

static ria_bel_t bel_queue[BEL_QUEUE_SIZE];
static volatile uint8_t bel_queue_head;
static volatile uint8_t bel_queue_tail;

static struct
{
    int16_t sample;
    uint8_t adsr;
    uint32_t vol;
    uint32_t phase;
    uint32_t noise1;
    uint32_t noise2;
    uint32_t elapsed_samples;
    volatile bool active;
} bel_state;

static void bel_put_sound(sst_cursor_t *c, const ria_bel_t *b)
{
    sst_put_u16(c, b->freq);
    sst_put_u8(c, b->duty);
    sst_put_u8(c, b->vol_attack);
    sst_put_u8(c, b->vol_decay);
    sst_put_u8(c, b->wave_release);
    sst_put_u16(c, b->restrike_ms);
    sst_put_u16(c, b->release_ms);
    sst_put_u16(c, b->end_ms);
}

static void bel_get_sound(sst_cursor_t *c, ria_bel_t *b)
{
    b->freq = sst_get_u16(c);
    b->duty = sst_get_u8(c);
    b->vol_attack = sst_get_u8(c);
    b->vol_decay = sst_get_u8(c);
    b->wave_release = sst_get_u8(c);
    b->restrike_ms = sst_get_u16(c);
    b->release_ms = sst_get_u16(c);
    b->end_ms = sst_get_u16(c);
}

void bel_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    for (int i = 0; i < BEL_QUEUE_SIZE; i++)
        bel_put_sound(c, &bel_queue[i]);
    sst_put_u8(c, bel_queue_head);
    sst_put_u8(c, bel_queue_tail);
    sst_put_u16(c, (uint16_t)bel_state.sample);
    sst_put_u8(c, bel_state.adsr);
    sst_put_u32(c, bel_state.vol);
    sst_put_u32(c, bel_state.phase);
    sst_put_u32(c, bel_state.noise1);
    sst_put_u32(c, bel_state.noise2);
    sst_put_u32(c, bel_state.elapsed_samples);
    sst_put_bool(c, bel_state.active);
}

bool bel_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    ria_bel_t queue[BEL_QUEUE_SIZE];
    for (int i = 0; i < BEL_QUEUE_SIZE; i++)
        bel_get_sound(c, &queue[i]);
    uint8_t head = sst_get_u8(c), tail = sst_get_u8(c);
    int16_t sample = (int16_t)sst_get_u16(c);
    uint8_t adsr = sst_get_u8(c);
    uint32_t vol = sst_get_u32(c), phase = sst_get_u32(c);
    uint32_t n1 = sst_get_u32(c), n2 = sst_get_u32(c);
    uint32_t elapsed = sst_get_u32(c);
    bool active = sst_get_bool(c);
    if (!sst_ok(c) || head >= BEL_QUEUE_SIZE || tail >= BEL_QUEUE_SIZE ||
        adsr > sustain)
        return false;
    memcpy(bel_queue, queue, sizeof bel_queue);
    bel_queue_head = head;
    bel_queue_tail = tail;
    bel_state.sample = sample;
    bel_state.adsr = adsr;
    bel_state.vol = vol;
    bel_state.phase = phase;
    bel_state.noise1 = n1;
    bel_state.noise2 = n2;
    bel_state.elapsed_samples = elapsed;
    bel_state.active = active;
    return true;
}

void bel_add(const ria_bel_t *sound)
{
    uint8_t next = (bel_queue_head + 1) % BEL_QUEUE_SIZE;
    if (next == bel_queue_tail)
        return;
    bel_queue[bel_queue_head] = *sound;
    bel_queue_head = next;

    if (!bel_state.active)
    {
        bel_state.adsr = attack;
        bel_state.vol = 0;
        bel_state.phase = 0;
        bel_state.elapsed_samples = 0;
        /* No barrier, though bel_sample may be on the audio thread. The
         * queue entry is written before bel_queue_head advances, and an idle
         * bel_sample returns at the active test without reading anything
         * else. Once it starts, every transition it makes waits on elapsed_ms
         * reaching restrike_ms, release_ms or end_ms, and one millisecond is
         * about fifty samples at 49716 Hz, so a field still in flight can
         * only alter the first sample or two of the note. */
        bel_state.active = true;
    }
}

#pragma GCC push_options
#pragma GCC optimize("O3")
int16_t bel_sample(void)
{
    if (!bel_state.active)
        return 0;

    ria_bel_t *snd = &bel_queue[bel_queue_tail];

    bel_state.elapsed_samples++;
    uint32_t elapsed_ms = (uint32_t)bel_state.elapsed_samples * 1000 / AUD_NATIVE_RATE;

    if (snd->restrike_ms > 0 && elapsed_ms >= snd->restrike_ms)
    {
        uint8_t next = (bel_queue_tail + 1) % BEL_QUEUE_SIZE;
        if (next != bel_queue_head)
        {
            ria_bel_t *next_snd = &bel_queue[next];
            if (next_snd->restrike_ms > 0)
            {
                bel_queue_tail = next;
                bel_state.adsr = attack;
                bel_state.vol = 0;
                bel_state.elapsed_samples = 0;
                snd = next_snd;
                goto generate;
            }
        }
    }

    if (snd->release_ms > 0 && elapsed_ms >= snd->release_ms &&
        bel_state.adsr != release)
    {
        bel_state.adsr = release;
    }

    if (snd->end_ms > 0 && elapsed_ms >= snd->end_ms)
    {
        uint8_t next = (bel_queue_tail + 1) % BEL_QUEUE_SIZE;
        if (next != bel_queue_head)
        {
            bel_queue_tail = next;
            snd = &bel_queue[bel_queue_tail];
            bel_state.adsr = attack;
            bel_state.vol = 0;
            bel_state.phase = 0;
            bel_state.elapsed_samples = 0;
        }
        else
        {
            bel_queue_tail = next;
            bel_state.active = false;
            return 0;
        }
    }

generate:;
    /* As psg.c, freq is Hertz times three, so the increment divides by
     * three times the sample rate. */
    uint32_t phase_inc = ((uint64_t)UINT32_MAX + 1) * snd->freq / 3 / AUD_NATIVE_RATE;
    bel_state.phase += phase_inc;
    uint32_t phase = bel_state.phase >> 24;
    uint32_t duty = snd->duty;

    switch (snd->wave_release >> 4)
    {
    case 0: // sine
        duty >>= 1;
        if (phase < 128u - duty || phase >= 128u + duty)
            bel_state.sample = BEL_RAIL;
        else
            bel_state.sample = sine_table[phase];
        break;
    case 1: // square
        if (phase > duty)
            bel_state.sample = BEL_RAIL;
        else
            bel_state.sample = BEL_PEAK;
        break;
    case 2: // sawtooth
        if (phase > duty)
            bel_state.sample = BEL_RAIL;
        else
            bel_state.sample = (int16_t)(BEL_PEAK - (int32_t)(bel_state.phase >> 16));
        break;
    case 3: // triangle
        duty >>= 1;
        if (phase < 128u - duty || phase >= 128u + duty)
            bel_state.sample = BEL_RAIL;
        else if (phase >= 128)
            bel_state.sample = (int16_t)(BEL_PEAK - (int16_t)(bel_state.phase >> 15));
        else
            bel_state.sample = (int16_t)((int16_t)(bel_state.phase >> 15) - 32768);
        break;
    case 4: // noise
        if (phase > duty)
            bel_state.sample = BEL_RAIL;
        else
        {
            bel_state.noise1 ^= bel_state.noise2;
            bel_state.sample = (int16_t)(bel_state.noise2 & 0xFFFF);
            bel_state.noise2 += bel_state.noise1;
        }
        break;
    default:
        bel_state.sample = 0;
        break;
    }

    uint32_t atk_rate = bel_attack_table[snd->vol_attack & 0xF];
    uint32_t atk_target = bel_vol_table[snd->vol_attack >> 4];
    uint32_t dec_rate = bel_decay_release_table[snd->vol_decay & 0xF];
    uint32_t dec_target = bel_vol_table[snd->vol_decay >> 4];
    uint32_t rel_rate = bel_decay_release_table[snd->wave_release & 0xF];

    switch (bel_state.adsr)
    {
    case attack:
        bel_state.vol += atk_rate;
        if (bel_state.vol >= atk_target)
        {
            bel_state.vol = atk_target;
            bel_state.adsr = decay;
        }
        break;
    case decay:
        if (bel_state.vol <= dec_rate)
            bel_state.vol = 0;
        else
            bel_state.vol -= dec_rate;
        if (bel_state.vol > dec_target)
            break;
        bel_state.adsr = sustain;
        __attribute__((fallthrough));
    case sustain:
        if (dec_target <= atk_target)
            bel_state.vol = dec_target;
        break;
    case release:
        if (bel_state.vol <= rel_rate)
            bel_state.vol = 0;
        else
            bel_state.vol -= rel_rate;
        break;
    }

    /* As psg.c: thirteen bits of the Q24 volume, rounded rather than
     * truncated. bel_vol_table peaks at 256 << 16, so a shift of twelve
     * makes full volume unity gain. */
    return (int16_t)(((int32_t)bel_state.sample
                          * (int32_t)(bel_state.vol >> 12)
                      + (1 << 11))
                     >> 12);
}
#pragma GCC pop_options

void bel_init(void)
{
    bel_state.noise1 = 0x67452301;
    bel_state.noise2 = 0xEFCDAB89;
}
