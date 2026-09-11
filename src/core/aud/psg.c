/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/aud/sine.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#define PSG_CHANNELS 8

/* The divisor the phase increments come out of. A channel's freq register
 * is Hertz times three, for a resolution of a third of a hertz, so an
 * increment divides by three times the sample rate. */
#define PSG_PHASE_DIV (3u * AUD_NATIVE_RATE)

/* Full scale of a generated wave, and the value a closed duty gate rails
 * to. The rail is -PSG_PEAK rather than the true minimum, so that it is
 * symmetric with the peak; psg.sv rails to the same -32767. */
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

/* Same rates as the 6581 SID, in milliseconds, as the increments they come
 * to at AUD_NATIVE_RATE. The rate multiplies before it divides by 1000
 * because AUD_NATIVE_RATE is not a whole number of kilohertz. psg.sv works
 * it out the same way. */
#define PSG_STEP(ms) ((1u << 24) / (uint32_t)(((uint64_t)AUD_NATIVE_RATE * (ms)) / 1000))

static const uint32_t psg_attack_table[16] = {
    PSG_STEP(2), PSG_STEP(8), PSG_STEP(16), PSG_STEP(24),
    PSG_STEP(38), PSG_STEP(56), PSG_STEP(68), PSG_STEP(80),
    PSG_STEP(100), PSG_STEP(250), PSG_STEP(500), PSG_STEP(800),
    PSG_STEP(1000), PSG_STEP(3000), PSG_STEP(5000), PSG_STEP(8000),
};

static const uint32_t psg_decay_release_table[16] = {
    PSG_STEP(6), PSG_STEP(24), PSG_STEP(48), PSG_STEP(72),
    PSG_STEP(114), PSG_STEP(168), PSG_STEP(204), PSG_STEP(240),
    PSG_STEP(300), PSG_STEP(750), PSG_STEP(1500), PSG_STEP(2400),
    PSG_STEP(3000), PSG_STEP(9000), PSG_STEP(15000), PSG_STEP(24000),
};

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
     * divides out to. The step loop divides only when the register has
     * changed, so a new note costs the divide and every sample after it
     * does not. Zero and zero is a correct pair, so no validity flag is
     * needed. */
    uint16_t freq;
    uint32_t phase_inc;
} psg_channel_state[PSG_CHANNELS];

#pragma GCC push_options
#pragma GCC optimize("O3")
void psg_sample(int16_t *left, int16_t *right)
{
    struct psg_channel *channels = (void *)&xram[psg_xaddr];

    /* The mix comes first, from the wave and envelope the last step left
     * and the registers as they stand now; the step comes after. That is
     * the order psg.sv walks in, and the lockstep test holds the two to the
     * sample.
     *
     * Both shifts round rather than truncate, because a floor is not noise
     * but a downward bias every sounding channel adds to. The envelope
     * takes thirteen bits of the Q24 volume: psg_vol_table peaks at
     * 256 << 16, so a shift of twelve makes full volume unity gain. */
    int32_t acc_l = 0;
    int32_t acc_r = 0;
    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        int32_t sample = ((int32_t)psg_channel_state[i].sample
                              * (int32_t)(psg_channel_state[i].vol >> 12)
                          + (1 << 11))
                         >> 12;
        int8_t pan = (int8_t)channels[i].pan_gate / 2;
        /* A pan_gate of 0x80 halves to -64, one past the documented
         * -63 to 63, and drops the channel from the mix. psg.sv tests for
         * the same value. */
        if (pan != -64)
        {
            acc_l += sample * (63 - pan);
            acc_r += sample * (63 + pan);
        }
    }
    /* The pan weights run from 0 to 126 against a shift of 128, so a
     * centred channel puts 63/128 on each side and a hard-panned one keeps
     * 126/128 of full scale. */
    acc_l = (acc_l + 64) >> 7;
    acc_r = (acc_r + 64) >> 7;
    if (acc_l < AUD_SAMPLE_MIN)
        acc_l = AUD_SAMPLE_MIN;
    if (acc_l > AUD_SAMPLE_MAX)
        acc_l = AUD_SAMPLE_MAX;
    if (acc_r < AUD_SAMPLE_MIN)
        acc_r = AUD_SAMPLE_MIN;
    if (acc_r > AUD_SAMPLE_MAX)
        acc_r = AUD_SAMPLE_MAX;
    *left = (int16_t)acc_l;
    *right = (int16_t)acc_r;

    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        if (channels[i].freq != psg_channel_state[i].freq)
        {
            psg_channel_state[i].freq = channels[i].freq;
            psg_channel_state[i].phase_inc =
                (uint32_t)(((uint64_t)channels[i].freq << 32) / PSG_PHASE_DIV);
        }
        psg_channel_state[i].phase += psg_channel_state[i].phase_inc;
        uint32_t phase = psg_channel_state[i].phase >> 24;
        uint32_t duty = channels[i].duty;
        switch (channels[i].wave_release >> 4)
        {
        case 0: // sine
            duty >>= 1;
            if (phase < 128u - duty || phase >= 128u + duty)
                psg_channel_state[i].sample = PSG_RAIL;
            else
                psg_channel_state[i].sample = sine_table[phase];
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

    /* Drain the writes the RW engine queued on this page. Only the gate
     * bit of pan_gate has to be caught as it happens, because it starts and
     * releases a note; every other byte is read from XRAM by the mix above,
     * where a change simply takes effect on the next sample. */
    uint8_t max_work = 32;
    while (max_work-- && xram_queue_tail != xram_queue_head)
    {
        /* Pairs with the release fence in ria.c: the entry is written
         * before the head that publishes it, and read after. */
        atomic_thread_fence(memory_order_acquire);
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

uint16_t psg_xaddr_get(void) { return psg_xaddr; }

void psg_park(void) { psg_xaddr = 0xFFFF; }

void psg_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u16(c, psg_xaddr);
    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        sst_put_u16(c, (uint16_t)psg_channel_state[i].sample);
        sst_put_u8(c, psg_channel_state[i].adsr);
        sst_put_u32(c, psg_channel_state[i].vol);
        sst_put_u32(c, psg_channel_state[i].phase);
        sst_put_u32(c, psg_channel_state[i].noise1);
        sst_put_u32(c, psg_channel_state[i].noise2);
        sst_put_u16(c, psg_channel_state[i].freq);
        sst_put_u32(c, psg_channel_state[i].phase_inc);
    }
}

bool psg_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint16_t at = sst_get_u16(c);
    __typeof__(psg_channel_state[0]) ch[PSG_CHANNELS];
    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        ch[i].sample = (int16_t)sst_get_u16(c);
        ch[i].adsr = sst_get_u8(c);
        ch[i].vol = sst_get_u32(c);
        ch[i].phase = sst_get_u32(c);
        ch[i].noise1 = sst_get_u32(c);
        ch[i].noise2 = sst_get_u32(c);
        ch[i].freq = sst_get_u16(c);
        ch[i].phase_inc = sst_get_u32(c);
        if (ch[i].adsr > sustain)
            return false;
    }
    /* psg_sample reads the 64-byte block at &xram[psg_xaddr] once a sample
     * with nothing guarding it, so a restored pointer has to pass the three
     * tests psg_xreg applies to a written one unless it is the parked
     * 0xFFFF: the address is even, because a channel begins with a uint16_t;
     * the block ends inside XRAM; and the block stays inside one page,
     * because psg.sv recognises a write to it by comparing pages. */
    if (!sst_ok(c) ||
        (at != 0xFFFF &&
         (at & 1 || at > 0x10000 - PSG_CHANNELS * sizeof(struct psg_channel) ||
          (at >> 8) != ((at + PSG_CHANNELS * sizeof(struct psg_channel) - 1) >> 8))))
        return false;
    psg_xaddr = at;
    for (unsigned i = 0; i < PSG_CHANNELS; i++)
        psg_channel_state[i] = ch[i];
    return true;
}

bool psg_xreg(uint16_t word)
{
    /* Taking the engine and giving it up both reset it, so a program never
     * inherits the last one's envelopes. The fabric does the same: psg
     * resets on any write to its pointer register, 0xFFFF included.
     *
     * Starting constants for the noise generator from
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
        /* Hand the mix back as well, because psg_sample would otherwise go
         * on reading the 64-byte channel block at 0xFFFF, which runs 63
         * bytes past the end of XRAM. */
        aud_unregister();
        return word == 0xFFFF;
    }
    psg_xaddr = word;
    xram_queue_page = word >> 8;
    xram_queue_tail = xram_queue_head;
    /* Taking the PSG releases the OPL's pointer, since aud_dev names one
     * device and only that one is mixed. */
    opl_park();
    aud_setup(aud_dev_psg);
    return true;
}
