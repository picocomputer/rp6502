/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/psg.h"
#include <stddef.h>
#include <string.h>

#define PSG_CHANNELS 8
/* The eight and the bell, which is the ninth voice here as in psg.sv. */
#define PSG_VOICES 9
#define PSG_BEL 8

/* Full scale of a generated wave, and the value a closed duty gate rails
 * to. The rail is -PSG_PEAK rather than the true minimum, so that it is
 * symmetric with the peak; psg.sv rails to the same -32767. */
#define PSG_PEAK 32767
#define PSG_RAIL (-32767)

/* The order is psg.sv's and the savestate's. */
enum
{
    PSG_ADSR_RELEASE,
    PSG_ADSR_ATTACK,
    PSG_ADSR_DECAY,
    PSG_ADSR_SUSTAIN,
};

/* Parked, so the eight are not mixed until a program maps the PSG. */
static uint16_t psg_xaddr = 0xFFFF;

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

/* Phase 0 is the trough and phase 128 the peak, because a voice indexes
 * this with the top byte of its phase and opens its duty gate around 128. */
static const int16_t psg_sine_table[256] = {
#include "aud_sine.h"
};

/* The constant-power pan law, out of 4096: a voice at pan p weighs
 * psg_pan_table[63 - p] on the left and psg_pan_table[63 + p] on the
 * right. The centre, 1024, puts a full sine at one OPL2 channel's level. */
static const uint16_t psg_pan_table[127] = {
#include "aud_pan.h"
};

/* A voice's freq holds hertz times three, so its phase increment is
 * freq * 2^32 / (3 * AUD_NATIVE_RATE), floored. The divide is a multiply by
 * the reciprocal rounded up, which gives the same result for every 16-bit
 * freq at both rates this is built for, 49716 Hz and the 48000 Hz of
 * tests/rtl/aud. psg.sv multiplies by a constant of its own, exact at
 * 48000 Hz. */
#define PSG_PHASE_MUL ((((uint64_t)1 << 63) + 3u * AUD_NATIVE_RATE - 1) / (3u * AUD_NATIVE_RATE))
static inline uint32_t psg_phase_inc(uint16_t freq)
{
    return (uint32_t)(((uint64_t)freq * PSG_PHASE_MUL) >> 31);
}

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

_Static_assert(PSG_CHANNELS * sizeof(struct psg_channel) == PSG_REGS_SIZE, "the block psg_sample reads");

/* The noise words are sixteen bits: an XOR and an add carry only upward,
 * and only noise2's low half is played. */
static struct
{
    int16_t sample;
    uint8_t adsr;
    uint32_t vol;
    uint32_t phase;
    uint16_t noise1;
    uint16_t noise2;
} psg_channel_state[PSG_VOICES] = {
    /* Nothing clears the bell's noise, so it starts seeded. */
    [PSG_BEL] = {.noise1 = 0x2301, .noise2 = 0xAB89},
};

/* The engine's own channel structures, as psg.sv's are: the program's
 * eight, which a map clears, then the bell's. Every write reaches them
 * through psg_put. XRAM is the program's and is never read. */
static struct psg_channel psg_regs[PSG_VOICES];

/* A silent bell with no frequency and no noise steps to the same state
 * every sample, so the mix skips it. psg.sv's walk steps it anyway, to the
 * same end. */
bool psg_bel_idle(void)
{
    return psg_channel_state[PSG_BEL].adsr == PSG_ADSR_RELEASE &&
           !psg_channel_state[PSG_BEL].vol && !psg_regs[PSG_BEL].freq &&
           (psg_regs[PSG_BEL].wave_release >> 4) != 4;
}

#pragma GCC push_options
#pragma GCC optimize("O3")
void psg_sample(int16_t *left, int16_t *right)
{
    /* The eight while a program maps them, then the bell while it sounds. */
    const unsigned lo = psg_xaddr != 0xFFFF ? 0 : PSG_BEL;
    const unsigned hi = psg_bel_idle() ? PSG_BEL : PSG_VOICES;

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
    for (unsigned i = lo; i < hi; i++)
    {
        const struct psg_channel *ch = &psg_regs[i];
        int32_t sample = ((int32_t)psg_channel_state[i].sample
                              * (int32_t)(psg_channel_state[i].vol >> 12)
                          + (1 << 11))
                         >> 12;
        int8_t pan = (int8_t)ch->pan_gate / 2;
        /* A pan_gate of 0x80 halves to -64, one past the documented
         * -63 to 63, and drops the channel from the mix. psg.sv reads
         * zero weights for the same value. */
        if (pan != -64)
        {
            acc_l += sample * psg_pan_table[63 - pan];
            acc_r += sample * psg_pan_table[63 + pan];
        }
    }
    /* Constant-power weights out of 4096: a centred channel puts 1/4 on
     * each side and a hard-panned one 0.354 on one, the same power. */
    acc_l = (acc_l + 2048) >> 12;
    acc_r = (acc_r + 2048) >> 12;
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

    for (unsigned i = lo; i < hi; i++)
    {
        const struct psg_channel *ch = &psg_regs[i];
        psg_channel_state[i].phase += psg_phase_inc(ch->freq);
        uint32_t phase = psg_channel_state[i].phase >> 24;
        uint32_t duty = ch->duty;
        switch (ch->wave_release >> 4)
        {
        case 0: // sine
            duty >>= 1;
            if (phase < 128u - duty || phase >= 128u + duty)
                psg_channel_state[i].sample = PSG_RAIL;
            else
                psg_channel_state[i].sample = psg_sine_table[phase];
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
        case PSG_ADSR_ATTACK:
            psg_channel_state[i].vol += psg_attack_table[ch->vol_attack & 0xF];
            if (psg_channel_state[i].vol >= psg_vol_table[ch->vol_attack >> 4])
            {
                psg_channel_state[i].vol = psg_vol_table[ch->vol_attack >> 4];
                psg_channel_state[i].adsr = PSG_ADSR_DECAY;
            }
            break;
        case PSG_ADSR_DECAY:
            if (psg_channel_state[i].vol <= psg_decay_release_table[ch->vol_decay & 0xF])
                psg_channel_state[i].vol = 0;
            else
                psg_channel_state[i].vol -= psg_decay_release_table[ch->vol_decay & 0xF];
            if (psg_channel_state[i].vol > psg_vol_table[ch->vol_decay >> 4])
                break;
            psg_channel_state[i].adsr = PSG_ADSR_SUSTAIN;
            __attribute__((fallthrough));
        case PSG_ADSR_SUSTAIN:
            if (psg_vol_table[ch->vol_decay >> 4] <= psg_vol_table[ch->vol_attack >> 4])
                psg_channel_state[i].vol = psg_vol_table[ch->vol_decay >> 4];
            break;
        case PSG_ADSR_RELEASE:
            if (psg_channel_state[i].vol <= psg_decay_release_table[ch->wave_release & 0xF])
                psg_channel_state[i].vol = 0;
            else
                psg_channel_state[i].vol -= psg_decay_release_table[ch->wave_release & 0xF];
            break;
        }
    }
}

/* Only the gate bit of pan_gate has to be caught as it happens, because it
 * starts and releases a note; every other byte lands in the structure and
 * takes effect on the next sample. */
static void psg_gate(unsigned i, uint8_t val)
{
    if (!(val & 0x01) && psg_channel_state[i].adsr != PSG_ADSR_RELEASE)
        psg_channel_state[i].adsr = PSG_ADSR_RELEASE;
    if ((val & 0x01) && psg_channel_state[i].adsr == PSG_ADSR_RELEASE)
        psg_channel_state[i].adsr = PSG_ADSR_ATTACK;
}

void psg_put(unsigned offset, uint8_t val)
{
    ((uint8_t *)psg_regs)[offset] = val;
    if (offset % sizeof(struct psg_channel) == offsetof(struct psg_channel, pan_gate))
        psg_gate(offset / sizeof(struct psg_channel), val);
}
#pragma GCC pop_options

uint16_t psg_xaddr_get(void) { return psg_xaddr; }

void psg_park(void) { psg_xaddr = 0xFFFF; }

void psg_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u16(c, psg_xaddr);
    for (unsigned i = 0; i < PSG_VOICES; i++)
    {
        sst_put_u16(c, (uint16_t)psg_channel_state[i].sample);
        sst_put_u8(c, psg_channel_state[i].adsr);
        sst_put_u32(c, psg_channel_state[i].vol);
        sst_put_u32(c, psg_channel_state[i].phase);
        sst_put_u16(c, psg_channel_state[i].noise1);
        sst_put_u16(c, psg_channel_state[i].noise2);
    }
    for (unsigned i = 0; i < sizeof psg_regs; i++)
        sst_put_u8(c, ((const uint8_t *)psg_regs)[i]);
}

bool psg_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint16_t at = sst_get_u16(c);
    __typeof__(psg_channel_state[0]) ch[PSG_VOICES];
    for (unsigned i = 0; i < PSG_VOICES; i++)
    {
        ch[i].sample = (int16_t)sst_get_u16(c);
        ch[i].adsr = sst_get_u8(c);
        ch[i].vol = sst_get_u32(c);
        ch[i].phase = sst_get_u32(c);
        ch[i].noise1 = sst_get_u16(c);
        ch[i].noise2 = sst_get_u16(c);
        if (ch[i].adsr > PSG_ADSR_SUSTAIN)
            return false;
    }
    uint8_t regs[sizeof psg_regs];
    for (unsigned i = 0; i < sizeof regs; i++)
        regs[i] = sst_get_u8(c);
    if (!sst_ok(c) || (at != 0xFFFF && !psg_block_ok(at)))
        return false;
    psg_xaddr = at;
    for (unsigned i = 0; i < PSG_VOICES; i++)
        psg_channel_state[i] = ch[i];
    memcpy(psg_regs, regs, sizeof regs);
    return true;
}

void psg_map(uint16_t word)
{
    /* Taking the engine and giving it up both reset it, so a program never
     * inherits the last one's envelopes. psg.sv does the same: it resets
     * on any write to its pointer register, 0xFFFF included. The phase goes
     * too, because psg.sv steps the eight while parked and this does not.
     *
     * The noise generator's starting constants are the low halves of
     * https://www.musicdsp.org/en/latest/Synthesis/216-fast-whitenoise-generator.html
     */
    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        psg_channel_state[i].noise1 = 0x2301;
        psg_channel_state[i].noise2 = 0xAB89;
        psg_channel_state[i].vol = 0;
        psg_channel_state[i].phase = 0;
        psg_channel_state[i].adsr = PSG_ADSR_RELEASE;
    }
    /* The registers start at zero, whatever XRAM holds. */
    memset(psg_regs, 0, PSG_REGS_SIZE);
    psg_xaddr = word;
}
