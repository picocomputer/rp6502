/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "core/aud/sine.h"

#if defined(DEBUG_AUD) || defined(DEBUG_AUD_BEL)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define BEL_QUEUE_SIZE 8

/* As psg.c: full scale, and the value a closed duty gate rails to. */
#define BEL_PEAK 32767
#define BEL_RAIL (-32767)

enum bel_adsr_state
{
    release,
    attack,
    decay,
    sustain,
};

/* Volume table: 16 levels in 16.16 fixed point.
 */

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

/* Same rates as the 6581 SID, in milliseconds, as the increments they come
 * to at AUD_NATIVE_RATE. (rate * ms) / 1000, not (rate / 1000) * ms: the old
 * form threw away the part of the rate below a kilohertz, which is exact at
 * 24000 and 48000 and 1.44% fast at 49716. Constants, as psg.c's are. */
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

/* Sound queue ring buffer.
 */

static ria_bel_t bel_queue[BEL_QUEUE_SIZE];
static volatile uint8_t bel_queue_head;
static volatile uint8_t bel_queue_tail;

/* Generator state.
 */

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

void bel_add(const ria_bel_t *sound)
{
    uint8_t next = (bel_queue_head + 1) % BEL_QUEUE_SIZE;
    if (next == bel_queue_tail)
        return; // Queue full, drop
    bel_queue[bel_queue_head] = *sound;
    bel_queue_head = next;

    // If not currently playing, start this sound
    if (!bel_state.active)
    {
        bel_state.adsr = attack;
        bel_state.vol = 0;
        bel_state.phase = 0;
        bel_state.elapsed_samples = 0;
        bel_state.active = true; // published last; a sampler on another thread sees consistent state
    }
}

#pragma GCC push_options
#pragma GCC optimize("O3")
int16_t bel_sample(void)
{
    if (!bel_state.active)
        return 0;

    ria_bel_t *snd = &bel_queue[bel_queue_tail];

    // Advance elapsed time and check timing events
    bel_state.elapsed_samples++;
    uint32_t elapsed_ms = (uint32_t)bel_state.elapsed_samples * 1000 / AUD_NATIVE_RATE;

    // Restrike when current and next both request it
    if (snd->restrike_ms > 0 && elapsed_ms >= snd->restrike_ms)
    {
        uint8_t next = (bel_queue_tail + 1) % BEL_QUEUE_SIZE;
        if (next != bel_queue_head)
        {
            ria_bel_t *next_snd = &bel_queue[next];
            if (next_snd->restrike_ms > 0)
            {
                // Restrike: advance to next sound, immediate attack
                bel_queue_tail = next;
                bel_state.adsr = attack;
                bel_state.vol = 0;
                bel_state.elapsed_samples = 0;
                snd = next_snd;
                goto generate;
            }
        }
    }

    // Check release_ms
    if (snd->release_ms > 0 && elapsed_ms >= snd->release_ms &&
        bel_state.adsr != release)
    {
        bel_state.adsr = release;
    }

    // Check end_ms: advance to next sound
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
            // No more sounds — consume the last entry so the slot is free
            bel_queue_tail = next;
            bel_state.active = false;
            return 0;
        }
    }

generate:;
    // Generate waveform sample
    uint32_t phase_inc = ((uint64_t)UINT32_MAX + 1) * snd->freq / 3 / AUD_NATIVE_RATE;
    bel_state.phase += phase_inc;
    uint32_t phase = bel_state.phase >> 24;
    uint32_t duty = snd->duty;

    /* Same generators as psg.c, and widened the same way: the duty gate
     * still compares the top byte so the shape is untouched, and the ramps
     * take eight more bits off the accumulator they were already using. */
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

    // Compute ADSR envelope
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

    /* Apply the envelope. Thirteen bits of it, rounded, and no second
     * shift afterwards — this used to truncate to eight and then re-add
     * the discarded bits as zeros on the way to the PWM. */
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
