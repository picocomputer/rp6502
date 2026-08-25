/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/aud.h"
#include "core/aud/bel.h"
#include "core/aud/psg.h"
#include "core/mem.h"
#include "host.h"
#include <stddef.h>
#include <string.h>

#if defined(DEBUG_RIA_AUD) || defined(DEBUG_RIA_AUD_PSG)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define PSG_CHANNELS 8

/* The rate psg_setup was last given, and the divisor the phase increments
 * come out of. Both derived once, because a runtime divisor in the sample
 * loop would be a real 64-bit division: eight of them per sample, against
 * 5,333 cycles at 48 kHz on a core that is also running the 6502's bus. */
static uint32_t psg_rate;
static uint32_t psg_phase_div;

/* Full scale of a generated wave, and the value a closed duty gate rails
 * to. The rail is -PSG_PEAK, not the true minimum, because it was -127
 * against a peak of 127 and this keeps that asymmetry rather than quietly
 * gaining an LSB of DC. */
#define PSG_PEAK 32767
#define PSG_RAIL (-32767)

enum psg_adsr_state
{
    release,
    attack,
    decay,
    sustain,
};

static volatile uint16_t psg_xaddr;

static const uint32_t psg_vol_table[] = {
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
static const uint16_t psg_attack_ms_table[] = {
    2, 8, 16, 24, 38, 56, 68, 80,
    100, 250, 500, 800, 1000, 3000, 5000, 8000,
};

static const uint16_t psg_decay_release_ms_table[] = {
    6, 24, 48, 72, 114, 168, 204, 240,
    300, 750, 1500, 2400, 3000, 9000, 15000, 24000,
};

/* The same rates as increments, once psg_setup knows what a second is.
 * 128 bytes of RAM where there used to be 128 bytes of flash. */
static uint32_t psg_attack_table[16];
static uint32_t psg_decay_release_table[16];

struct psg_channel
{
    uint16_t freq;
    uint8_t duty;
    uint8_t vol_attack;
    uint8_t vol_decay;
    uint8_t wave_release;
    uint8_t pan_gate;
    uint8_t unused;
};

static struct
{
    int16_t sample;
    uint8_t adsr;
    uint32_t vol;
    uint32_t phase;
    uint32_t noise1;
    uint32_t noise2;
    /* The last frequency this channel was written and the increment it
     * divides out to. The division is the expensive thing in the sample
     * loop and freq only moves when a program writes a note, so it is done
     * then. Zero and zero is a correct pair — freq 0 really does give
     * increment 0 — so no separate validity flag is needed. */
    uint16_t freq;
    uint32_t phase_inc;
} psg_channel_state[PSG_CHANNELS];

void psg_setup(uint32_t rate)
{
    psg_rate = rate;
    /* One divisor rather than two divisions per channel per sample.
     * floor(floor(x/3)/rate) is floor(x/(3*rate)) for positive integers, so
     * this is the same number the RTL's restoring divider produces from the
     * same constant. */
    psg_phase_div = 3 * rate;
    for (unsigned i = 0; i < 16; i++)
    {
        /* (rate * ms) / 1000, not (rate / 1000) * ms: the old form was exact
         * at 24000 and 48000 and 0.23% slow at 44100, which is a rate a
         * sound card can hand back. */
        psg_attack_table[i] =
            (1 << 24) / (uint32_t)(((uint64_t)rate * psg_attack_ms_table[i]) / 1000);
        psg_decay_release_table[i] =
            (1 << 24) / (uint32_t)(((uint64_t)rate * psg_decay_release_ms_table[i]) / 1000);
    }
    /* The cached increments were divided by the old rate. */
    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        psg_channel_state[i].freq = 0;
        psg_channel_state[i].phase_inc = 0;
    }
}

#pragma GCC push_options
#pragma GCC optimize("O3")
static void
    HOST_ISR
    HOST_TIME_CRITICAL(psg_irq_handler)(void)
{
    aud_clear_irq();

    struct psg_channel *channels = (void *)&xram[psg_xaddr];

    /* Output previous sample at start to minimize jitter.
     *
     * The mix accumulates unshifted and rounds once. It used to truncate
     * twice — after the envelope and again after the pan — and a floor is
     * not noise, it is a downward bias that every sounding channel adds to.
     * Eight of them reached -26 dBFS of DC that appeared and vanished with
     * the notes. The envelope takes thirteen bits here rather than nine:
     * psg_vol_table is Q24 and at nine bits a long release moved in 256
     * visible steps, which is the zipper you could hear on a slow fade. */
    int32_t acc_l = 0;
    int32_t acc_r = 0;
    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        int32_t sample = ((int32_t)psg_channel_state[i].sample
                              * (int32_t)(psg_channel_state[i].vol >> 12)
                          + (1 << 11))
                         >> 12;
        int8_t pan = (int8_t)channels[i].pan_gate / 2;
        if (pan != -64)
        {
            acc_l += sample * (63 - pan);
            acc_r += sample * (63 + pan);
        }
    }
    /* 63/64 rather than 1/2 per side is the pan law this has always had;
     * the shift undoes it and lands on full scale. */
    int32_t bel_mix = bel_sample(psg_rate);
    acc_l = ((acc_l + 64) >> 7) + bel_mix;
    acc_r = ((acc_r + 64) >> 7) + bel_mix;
    if (acc_l < AUD_SAMPLE_MIN)
        acc_l = AUD_SAMPLE_MIN;
    if (acc_l > AUD_SAMPLE_MAX)
        acc_l = AUD_SAMPLE_MAX;
    if (acc_r < AUD_SAMPLE_MIN)
        acc_r = AUD_SAMPLE_MIN;
    if (acc_r > AUD_SAMPLE_MAX)
        acc_r = AUD_SAMPLE_MAX;
    aud_out((int16_t)acc_l, (int16_t)acc_r);

    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        if (channels[i].freq != psg_channel_state[i].freq)
        {
            psg_channel_state[i].freq = channels[i].freq;
            psg_channel_state[i].phase_inc =
                (uint32_t)(((uint64_t)channels[i].freq << 32) / psg_phase_div);
        }
        psg_channel_state[i].phase += psg_channel_state[i].phase_inc;
        /* The duty gate still compares the top byte of the phase, so where
         * a wave starts and stops is unchanged. Only the value widens: the
         * ramps take eight more bits off the same accumulator, so they are
         * the same slope with a finer staircase, and PSG_RAIL is the old
         * -127 rail scaled. The gate rails to full negative rather than to
         * silence, which is how duty has always worked here. */
        uint32_t phase = psg_channel_state[i].phase >> 24;
        uint32_t duty = channels[i].duty;
        switch (channels[i].wave_release >> 4)
        {
        case 0: // sine
            duty >>= 1;
            if (phase < 128u - duty || phase >= 128u + duty)
                psg_channel_state[i].sample = PSG_RAIL;
            else
                psg_channel_state[i].sample = aud_sine_table[phase];
            break;
        case 1: // square
            if (phase > duty)
                psg_channel_state[i].sample = PSG_RAIL;
            else
                psg_channel_state[i].sample = PSG_PEAK;
            break;
        case 2: // sawtooth
            if (phase > duty)
                psg_channel_state[i].sample = PSG_RAIL;
            else
                psg_channel_state[i].sample =
                    (int16_t)(PSG_PEAK - (int32_t)(psg_channel_state[i].phase >> 16));
            break;
        case 3: // triangle
            duty >>= 1;
            if (phase < 128u - duty || phase >= 128u + duty)
                psg_channel_state[i].sample = PSG_RAIL;
            else if (phase >= 128)
                psg_channel_state[i].sample =
                    (int16_t)(PSG_PEAK - (int16_t)(psg_channel_state[i].phase >> 15));
            else
                psg_channel_state[i].sample =
                    (int16_t)((int16_t)(psg_channel_state[i].phase >> 15) - 32768);
            break;
        case 4: // noise
            if (phase > duty)
                psg_channel_state[i].sample = PSG_RAIL;
            else
            {
                psg_channel_state[i].noise1 ^= psg_channel_state[i].noise2;
                psg_channel_state[i].sample =
                    (int16_t)(psg_channel_state[i].noise2 & 0xFFFF);
                psg_channel_state[i].noise2 += psg_channel_state[i].noise1;
            }
            break;
        default:
            psg_channel_state[i].sample = 0;
            break;
        }

        // Compute the ADSR envelope volume
        switch (psg_channel_state[i].adsr)
        {
        case attack:
            psg_channel_state[i].vol += psg_attack_table[channels[i].vol_attack & 0xF];
            if (psg_channel_state[i].vol >= psg_vol_table[channels[i].vol_attack >> 4])
            {
                psg_channel_state[i].vol = psg_vol_table[channels[i].vol_attack >> 4];
                psg_channel_state[i].adsr = decay;
            }
            break;
        case decay:
            if (psg_channel_state[i].vol <= psg_decay_release_table[channels[i].vol_decay & 0xF])
                psg_channel_state[i].vol = 0;
            else
                psg_channel_state[i].vol -= psg_decay_release_table[channels[i].vol_decay & 0xF];
            if (psg_channel_state[i].vol > psg_vol_table[channels[i].vol_decay >> 4])
                break;
            psg_channel_state[i].adsr = sustain;
            __attribute__((fallthrough));
        case sustain:
            if (psg_vol_table[channels[i].vol_decay >> 4] <= psg_vol_table[channels[i].vol_attack >> 4])
                psg_channel_state[i].vol = psg_vol_table[channels[i].vol_decay >> 4];
            break;
        case release:
            if (psg_channel_state[i].vol <= psg_decay_release_table[channels[i].wave_release & 0xF])
                psg_channel_state[i].vol = 0;
            else
                psg_channel_state[i].vol -= psg_decay_release_table[channels[i].wave_release & 0xF];
            break;
        }
    }

    // Detect gate changes using xram_queue
    uint8_t max_work = 32;
    while (max_work-- && xram_queue_tail != xram_queue_head)
    {
        uint8_t tail = ++xram_queue_tail;
        uint8_t loc = xram_queue[tail][0];
        uint8_t val = xram_queue[tail][1];
        uint16_t xaddr = (psg_xaddr & 0xFF00) + loc;
        uint16_t offset = xaddr - psg_xaddr;
        if ((offset % sizeof(struct psg_channel)) == offsetof(struct psg_channel, pan_gate))
        {
            unsigned i = offset / sizeof(struct psg_channel);
            if (i < PSG_CHANNELS)
            {
                if (!(val & 0x01) && psg_channel_state[i].adsr != release)
                    psg_channel_state[i].adsr = release;
                if ((val & 0x01) && psg_channel_state[i].adsr == release)
                    psg_channel_state[i].adsr = attack;
            }
        }
    }
}
#pragma GCC pop_options

bool psg_xreg(uint16_t word)
{
    /* Taking control and giving it up both reset the engine, the way a
     * reset line would, so a program never inherits the last one's
     * envelopes. The fabric has always done this — aud_psg resets on any
     * write to its pointer register, 0xFFFF included — and this is the
     * software catching up.
     *
     * Starting constants for the noise LFSR from here:
     * https://www.musicdsp.org/en/latest/Synthesis/216-fast-whitenoise-generator.html
     */
    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        psg_channel_state[i].noise1 = 0x67452301;
        psg_channel_state[i].noise2 = 0xEFCDAB89;
        psg_channel_state[i].vol = 0;
        psg_channel_state[i].adsr = release;
        psg_channel_state[i].freq = 0;
        psg_channel_state[i].phase_inc = 0;
    }
    if (word & 0x0001 ||
        word > 0x10000 - PSG_CHANNELS * sizeof(struct psg_channel) ||
        ((word >> 8) != ((word + PSG_CHANNELS * sizeof(struct psg_channel) - 1) >> 8)))
    {
        psg_xaddr = 0xFFFF;
        /* And hand the interrupt back. The handler reads its channel
         * block from &xram[psg_xaddr] with nothing guarding it, so a
         * parked pointer left installed walks 64 bytes off the end of
         * XRAM once a sample, forever. */
        aud_stop();
        return word == 0xFFFF;
    }
    psg_xaddr = word;
    xram_queue_page = word >> 8;
    xram_queue_tail = xram_queue_head;
    aud_setup(psg_irq_handler, psg_rate);
    return true;
}
