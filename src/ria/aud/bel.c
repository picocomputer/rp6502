/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "aud/aud.h"
#include "aud/bel.h"
#include <pico/stdlib.h>
#include <hardware/pwm.h>

#if defined(DEBUG_RIA_AUD) || defined(DEBUG_RIA_AUD_BEL)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define BEL_DEFAULT_RATE 24000
#define BEL_QUEUE_SIZE 8

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

// Same rates as the 6581 SID, in milliseconds
static const uint16_t bel_attack_ms_table[] = {
    2,    // 0
    8,    // 1
    16,   // 2
    24,   // 3
    38,   // 4
    56,   // 5
    68,   // 6
    80,   // 7
    100,  // 8
    250,  // 9
    500,  // A
    800,  // B
    1000, // C
    3000, // D
    5000, // E
    8000, // F
};

static const uint16_t bel_decay_release_ms_table[] = {
    6,     // 0
    24,    // 1
    48,    // 2
    72,    // 3
    114,   // 4
    168,   // 5
    204,   // 6
    240,   // 7
    300,   // 8
    750,   // 9
    1500,  // A
    2400,  // B
    3000,  // C
    9000,  // D
    15000, // E
    24000, // F
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
    int8_t sample;
    uint8_t adsr;
    uint32_t vol;
    uint32_t phase;
    uint32_t noise1;
    uint32_t noise2;
    uint32_t elapsed_samples;
    volatile bool active;
} bel_state;

// Teletype bell: restrike-capable
__in_flash("bel_presets") const ria_bel_t bel_teletype = {
    .freq = 1760,
    .duty = 215,          // hint of grit
    .vol_attack = 0x51,   // attack to -5vol in 8ms
    .vol_decay = 0x60,    // decay to -6vol in 6ms
    .wave_release = 0x39, // triangle wave, release to zero in 750ms
    .restrike_ms = 100,   // restrike 10 Hz
    .release_ms = 20,
    .end_ms = 800,
};

// NFC fail/error: low square buzz
__in_flash("bel_presets") const ria_bel_t bel_nfc_fail = {
    .freq = 330,
    .duty = 127,          // 50% square
    .vol_attack = 0x80,   // attack to -8vol in 2ms
    .vol_decay = 0x80,    // sustain at -8vol
    .wave_release = 0x15, // square, release to zero in 168ms
    .restrike_ms = 0,
    .release_ms = 200,
    .end_ms = 420,
};

// NFC success note 1
__in_flash("bel_presets") const ria_bel_t bel_nfc_success_1 = {
    .freq = 784,
    .duty = 255,          // full cycle
    .vol_attack = 0x60,   // attack to -6vol in 2ms
    .vol_decay = 0x60,    // sustain at -6vol
    .wave_release = 0x03, // sine, release to zero in 72ms
    .restrike_ms = 0,
    .release_ms = 90,
    .end_ms = 170,
};

// NFC success note 2
__in_flash("bel_presets") const ria_bel_t bel_nfc_success_2 = {
    .freq = 1568,
    .duty = 255,          // full cycle
    .vol_attack = 0x60,   // attack to -6vol in 2ms
    .vol_decay = 0x60,    // sustain at -6vol
    .wave_release = 0x06, // sine, release to zero in 204ms
    .restrike_ms = 0,
    .release_ms = 130,
    .end_ms = 350,
};

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
        bel_state.active = true; // published last; IRQ always sees consistent state
    }
}

static inline uint32_t bel_attack_rate(uint8_t nibble, uint32_t rate)
{
    return (1 << 24) / (rate / 1000 * bel_attack_ms_table[nibble]);
}

static inline uint32_t bel_decay_release_rate(uint8_t nibble, uint32_t rate)
{
    return (1 << 24) / (rate / 1000 * bel_decay_release_ms_table[nibble]);
}

__attribute__((optimize("O3"))) int16_t
__time_critical_func(bel_sample)(uint32_t rate)
{
    if (!bel_state.active)
        return 0;

    ria_bel_t *snd = &bel_queue[bel_queue_tail];

    // Advance elapsed time and check timing events
    bel_state.elapsed_samples++;
    uint32_t elapsed_ms = (uint32_t)bel_state.elapsed_samples * 1000 / rate;

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
    uint32_t phase_inc = ((uint64_t)UINT32_MAX + 1) * snd->freq / 3 / rate;
    bel_state.phase += phase_inc;
    uint32_t phase = bel_state.phase >> 24;
    uint32_t duty = snd->duty;

    switch (snd->wave_release >> 4)
    {
    case 0: // sine
        duty >>= 1;
        if (phase < 128u - duty || phase >= 128u + duty)
            bel_state.sample = -127;
        else
            bel_state.sample = aud_sine_table[phase];
        break;
    case 1: // square
        if (phase > duty)
            bel_state.sample = -127;
        else
            bel_state.sample = 127;
        break;
    case 2: // sawtooth
        if (phase > duty)
            bel_state.sample = -127;
        else
            bel_state.sample = 127 - phase;
        break;
    case 3: // triangle
        duty >>= 1;
        if (phase < 128u - duty || phase >= 128u + duty)
            bel_state.sample = -127;
        else if (phase >= 128)
            bel_state.sample = 127 - (int8_t)(bel_state.phase >> 23);
        else
            bel_state.sample = (int8_t)(bel_state.phase >> 23) - 128;
        break;
    case 4: // noise
        if (phase > duty)
            bel_state.sample = -127;
        else
        {
            bel_state.noise1 ^= bel_state.noise2;
            bel_state.sample = bel_state.noise2 & 0xFF;
            bel_state.noise2 += bel_state.noise1;
        }
        break;
    default:
        bel_state.sample = 0;
        break;
    }

    // Compute ADSR envelope
    uint32_t atk_rate = bel_attack_rate(snd->vol_attack & 0xF, rate);
    uint32_t atk_target = bel_vol_table[snd->vol_attack >> 4];
    uint32_t dec_rate = bel_decay_release_rate(snd->vol_decay & 0xF, rate);
    uint32_t dec_target = bel_vol_table[snd->vol_decay >> 4];
    uint32_t rel_rate = bel_decay_release_rate(snd->wave_release & 0xF, rate);

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

    // Apply envelope to sample
    int16_t out = ((int32_t)bel_state.sample * (bel_state.vol >> 16)) >> 8;
    // Scale to PWM bits
    out <<= (AUD_PWM_BITS - 8);
    return out;
}

static __isr __attribute__((optimize("O3"))) void
__time_critical_func(bel_irq_handler)(void)
{
    pwm_clear_irq(AUD_IRQ_SLICE);

    int16_t sample = bel_sample(BEL_DEFAULT_RATE);

    int16_t max_val = (1 << (AUD_PWM_BITS - 1)) - 1;
    int16_t min_val = -(1 << (AUD_PWM_BITS - 1));
    if (sample < min_val)
        sample = min_val;
    if (sample > max_val)
        sample = max_val;

    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, sample + AUD_PWM_CENTER);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, sample + AUD_PWM_CENTER);
}

void bel_setup(void)
{
    bel_state.noise1 = 0x67452301;
    bel_state.noise2 = 0xEFCDAB89;
    aud_setup(bel_irq_handler, BEL_DEFAULT_RATE);
}
